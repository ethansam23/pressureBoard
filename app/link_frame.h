#ifndef LINK_FRAME_H
#define LINK_FRAME_H

/*******************************************************************************
 * Downhole link — pure protocol core (wire format + packet state machine).
 *
 * This module is deliberately hardware-free: no SDK includes, stdint/stdbool
 * types only. It compiles unchanged under gcc for the host test harness in
 * host_tests/ — which is the only executable verification this protocol gets
 * before bench day, so KEEP IT PURE. The UART/ISR shim lives in link_tx.c.
 *
 * Wire format (fixed by the logger designer — never change unilaterally):
 *
 *   |0x7F| >2ms gap |MSB|LSB|CHK|   ...>20ms idle...   repeat
 *
 *   CHK = ~(MSB+LSB) (8-bit additive checksum — NOT a CRC: it misses
 *   MSB/LSB transposition and compensating byte errors, and accepts random
 *   corruption ~1/256; redundancy comes from continuous retransmission).
 *   9600 baud 8N1. Whole packet < 10 ms. Period = sync-start -> sync-start.
 *
 * PAYLOAD IS SACRED: codes are transmitted exactly as encoded. 0x7F is legal
 * in the LSB and checksum positions — no escaping, no value nudging. Framing
 * is assumed gap-based per the logger's timing diagram (external gate).
 *
 * Timing note: the shim timestamps bytes at the UART TI interrupt. Working
 * hypothesis (unproven in local docs — hardware gate): TI may assert at the
 * stop-bit START, ~1 bit-time before the frame fully leaves the pin. All
 * idle arithmetic therefore adds a 1-tick allowance on top of TI ticks.
 ******************************************************************************/

#include <stdint.h>
#include <stdbool.h>

/* ---- Wire protocol constants --------------------------------------------- */
#define LINK_SYNC_BYTE          0x7Fu

#define LINK_VALUE_MAX          10000u   /* 1000.0 bar at 0.1 bar/LSB         */

/* Status codes (0xFF00 page). Low byte = reason. NEVER assign 0xFF7F or
 * 0xFF81 (their wire image would contain 0x7F — assignment hygiene only;
 * payload bytes may legally be 0x7F).                                        */
#define LINK_CODE_NO_READING    0xFF01u  /* boot: no sample captured yet      */
#define LINK_CODE_UNCAL         0xFF02u  /* no valid calibration / non-finite */
#define LINK_CODE_DISAGREE      0xFF03u  /* probe disagreement                */
#define LINK_CODE_ADC_STALL     0xFF04u  /* ADC conversions dead              */
#define LINK_CODE_VDDEXT        0xFF05u  /* excitation rail unstable          */
#define LINK_CODE_OVER_RANGE    0xFF06u  /* pressure > 1010.0 bar             */
#define LINK_CODE_UNDER_RANGE   0xFF07u  /* pressure < -5.0 bar               */

/* Encode policy edges (float domain, checked before any integer conversion) */
#define LINK_FULL_SCALE_BAR     1000.0f
#define LINK_OVER_LIMIT_BAR     1010.0f  /* (1000,1010] clamps to 10000       */
#define LINK_UNDER_LIMIT_BAR    (-5.0f)  /* [-5,0) clamps to 0                */

/* ---- Timing constants (1 ms tick domain) ---------------------------------
 * Derivations (9600 8N1: byte = 1041.7 us, bit = 104.2 us):
 *  - GAP 4:      real sync-stop-end -> data-start gap = 2.90..4.9 ms (>2 ok)
 *  - DEADLINE 5: data admitted only while (now - sync_start) <= 5 ticks
 *                => absolute packet (sync start bit -> checksum stop end)
 *                <= 5.999 + 3.175 = 9.17 ms  (0.83 ms margin below 10)
 *  - PERIOD 110: ~9.1 pkt/s nominal; rebased on ACTUAL sync start (no catch-up).
 *                Nominal wire idle = 110 - 9.17 = ~100.8 ms. Was 40 ms
 *                (25 pkt/s) through step 4b; stretched on request to put
 *                ~100 ms between packets instead of riding the protocol's
 *                20 ms floor. NOTE: this cuts the 2x-rule logger ceiling
 *                from 12.5 Hz to ~4.5 Hz and GATE Q7 is still open -- see
 *                link_protocol.md 4.
 *  - IDLE 22:    UNCHANGED. This guards the logger's >20 ms floor, which is
 *                logger-defined and not ours to move; measured from the TI
 *                tick of the last byte => real wire idle >= ~20.9 ms even at
 *                worst tick quantization. It stays well under the ~100.8 ms
 *                nominal, so the PERIOD drives the cadence and this remains a
 *                safety net for overdue/perturbed packets.
 ******************************************************************************/
#define LINK_PACKET_PERIOD_MS   110u
#define LINK_SYNC_GAP_MS        4u
#define LINK_DATA_DEADLINE_MS   5u
#define LINK_IDLE_MIN_MS        22u
#define LINK_SYNC_TIMEOUT_MS    3u   /* TI expected ~0.94ms after queue       */
#define LINK_DATA_TIMEOUT_MS    8u   /* burst is ~3.2ms of wire time          */

/* ---- Pure helpers --------------------------------------------------------- */
uint8_t  link_checksum(uint8_t msb, uint8_t lsb);      /* ~(msb+lsb), mod 256 */
uint16_t link_encode_bar(float bar);                   /* policy above        */
void     link_build_data(uint16_t code, uint8_t out[3]); /* MSB, LSB, CHK     */

/* ---- Packet state machine ------------------------------------------------- */
typedef enum
{
    LINK_ST_IDLE = 0,   /* between packets                                    */
    LINK_ST_SYNC,       /* sync byte queued / in flight                       */
    LINK_ST_GAP,        /* sync sent; waiting out the >2ms gap                */
    LINK_ST_DATA,       /* 3-byte data block queued / in flight               */
    LINK_ST_RECOVERY    /* post-abort quiet-down (full idle-min from 'now')   */
} link_state_t;

typedef enum
{
    LINK_ACT_NONE = 0,
    LINK_ACT_SEND_SYNC, /* caller transmits LINK_SYNC_BYTE now                */
    LINK_ACT_SEND_DATA  /* caller transmits sm->data[3] back-to-back now      */
} link_action_t;

/* Abort origins. Every abort takes the conservative recovery path: the
 * last-activity reference becomes 'now' (wire state uncertain) and a FULL
 * idle-min must elapse before the next sync. Worst case on the wire is a
 * sync byte followed by silence — never a partial data block.               */
typedef enum
{
    LINK_ABT_SYNC_TIMEOUT = 0,  /* TI never arrived for the sync byte        */
    LINK_ABT_DATA_DEADLINE,     /* gap phase overran the packet deadline     */
    LINK_ABT_DATA_TIMEOUT,      /* TI never arrived for the data burst       */
    LINK_ABT_BAD_STATE,         /* SM in an impossible state                 */
    LINK_ABT_EXTERNAL,          /* shim-detected (e.g. stale queued byte)    */
    LINK_ABT_COUNT
} link_abort_t;

typedef struct
{
    link_state_t st;
    uint32_t t_sync_start;   /* tick sync was queued (= packet/period ref)   */
    uint32_t t_prev_sync;    /* previous ACTUAL sync start (period base)     */
    uint32_t t_last_ti;      /* TI tick of most recent packet byte           */
    uint32_t t_quiet_ref;    /* base for idle-min (TI tick, or 'now' after
                              * an abort — conservative)                     */
    uint16_t code;           /* code latched for the in-flight packet        */
    uint8_t  data[3];        /* MSB, LSB, CHK built at latch time            */
    uint32_t pkts_ok;
    uint16_t aborts[LINK_ABT_COUNT];
    uint16_t busy_skips;     /* sync due but UART not idle                   */
} link_sm_t;

/* Inputs sampled once per service pass. tx_done is a consume-once event set
 * by the shim when the ISR finished a queued packet transmission (sync byte
 * or final burst byte); tx_done_tick is scheduler_get_ms() captured in the
 * ISR at that TI.                                                            */
typedef struct
{
    uint32_t now;
    uint16_t live_code;      /* latched only at packet start                 */
    bool     tx_done;
    uint32_t tx_done_tick;
    bool     uart_idle;      /* hardware idle: no byte in flight             */
    bool     held;           /* fence hold: no NEW packet may start          */
    bool     packet_mode;    /* false while bench console owns the line      */
} link_sm_in_t;

void          link_sm_init(link_sm_t *sm, uint32_t now);
link_action_t link_sm_step(link_sm_t *sm, const link_sm_in_t *in);

/* Shim-detected fault (e.g. stale byte in the burst queue): force the
 * conservative recovery path with origin accounting.                        */
void link_sm_force_recovery(link_sm_t *sm, link_abort_t origin, uint32_t now);

/* True while a packet is on the wire or committed (SYNC/GAP/DATA) — the
 * window in which nothing else may touch the UART.                          */
bool link_sm_in_packet(const link_sm_t *sm);

/* ===========================================================================
 *  Bench simulation profile — synthetic pressure source (BENCH BUILDS ONLY)
 * ===========================================================================
 * Stands in for the transducers so the link path can be verified in isolation
 * from the ADC and the calibration math. Kept here, in the pure protocol core,
 * for one reason: host_tests/ compiles this file under gcc, so the host's
 * reference stream is produced by the EXACT function the firmware runs. No
 * reimplementation, no soft-float divergence, no "did I mirror it correctly".
 *
 * Integer-only by construction — never introduce a float into this section.
 *
 * The profile has two phases (see verification_guide.md):
 *
 *   Phase A — resolution sweep, 20,600 s. Ramps 0 -> 10000 -> 0 deci-bar at
 *             one code per refresh, so every one of the 10,001 valid codes is
 *             transmitted once ascending and once descending. This is also
 *             what covers the payload-sacred cases (every code whose LSB or
 *             checksum lands on 0x7F) without a hand-picked vector list.
 *
 *   Phase B — ramp-timing ladder, 3,600 s per cycle, 18 cycles. Five tiers of
 *             FIXED-DURATION ramp windows (5 min, 2 min, 1 min, 30 s, 10 s),
 *             four ramps each at differing slew rates: 1..1000 dbar/refresh.
 *
 *   Full run = A + 18*B + a 1,000 s stop = 86,400,000 ms exactly (24 h).
 *
 * Segment durations are WALL-CLOCK MILLISECONDS, resolved to refresh counts
 * against the active RATE. That is deliberate: the ladder measures durations,
 * so a RATE change must not silently rescale the windows. Consequence — RATE
 * must not change mid-run, and rate_ms > 1000 degrades Phase A's resolution
 * (at RATE 5000 the sweep steps 5 dbar and skips codes). rate_ms <= 1000 gives
 * full coverage; faster rates simply hold each code for more refreshes.
 ******************************************************************************/

/* Bench-only: compiled out entirely unless the build defines APP_ENABLE_SIM=1
 * (Keil target define for bench builds; -DAPP_ENABLE_SIM=1 in host_tests).
 * Driven from the compiler define rather than app_config.h so this file stays
 * standalone-compilable, and so every translation unit agrees.              */
#ifndef APP_ENABLE_SIM
#define APP_ENABLE_SIM          0
#endif

#if APP_ENABLE_SIM

/* Which portion of the profile to run. */
#define SIM_PHASE_FULL          0u   /* A, then 18x B, then the closing stop  */
#define SIM_PHASE_A             1u   /* resolution sweep alone                */
#define SIM_PHASE_B             2u   /* one ladder cycle alone                */

/* Segment kinds (sim_seg_info_t.kind). */
#define SIM_SEG_STATUS          0u   /* walks LINK_CODE_NO_READING..UNDER_RANGE */
#define SIM_SEG_RAMP            1u   /* linear interpolation to .target       */
#define SIM_SEG_HOLD            2u   /* constant .target                      */

#define SIM_STATUS_CODES        7u   /* FF01..FF07                            */
#define SIM_PHASE_B_CYCLES      18u  /* ladder repeats in a full 24 h run     */

/* One resolved segment, for STATUS reporting and for the host verifier's
 * per-ramp duration checks. */
typedef struct
{
    uint8_t  kind;
    uint16_t start;      /* code at segment entry (RAMP start / HOLD value)   */
    uint16_t target;     /* code at segment exit                              */
    uint32_t first;      /* first refresh index belonging to this segment     */
    uint32_t steps;      /* refresh count = dur_ms / rate_ms                  */
    uint32_t dur_ms;     /* specified wall-clock duration                     */
} sim_seg_info_t;

/* The wire code for refresh `index` of `phase`, at the given refresh period.
 * Pure: same inputs always give the same output. `index` beyond the profile
 * length wraps. rate_ms of 0 is treated as 1 to stay total.                  */
uint16_t sim_profile_code(uint32_t index, uint32_t rate_ms, uint8_t phase);

/* Total refresh count of `phase` at `rate_ms` (the wrap period).             */
uint32_t sim_profile_len(uint32_t rate_ms, uint8_t phase);

/* Segment table introspection. seg is 0-based within the phase; for
 * SIM_PHASE_FULL the Phase B cycle segments repeat, so `first` reflects the
 * cycle given by seg / segments-per-cycle. Returns false when seg is out of
 * range.                                                                     */
uint32_t sim_profile_seg_count(uint8_t phase);
bool     sim_profile_seg_info(uint8_t phase, uint32_t seg, uint32_t rate_ms,
                              sim_seg_info_t *out);

/* Index of the segment containing `index` (companion to seg_info).           */
uint32_t sim_profile_seg_at(uint32_t index, uint32_t rate_ms, uint8_t phase);

#endif /* APP_ENABLE_SIM */

#endif /* LINK_FRAME_H */
