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
 *  - PERIOD 40:  25 pkt/s nominal; rebased on ACTUAL sync start (no catch-up)
 *  - IDLE 22:    measured from the TI tick of the last byte => real wire
 *                idle >= ~20.9 ms even at worst tick quantization
 ******************************************************************************/
#define LINK_PACKET_PERIOD_MS   40u
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

#endif /* LINK_FRAME_H */
