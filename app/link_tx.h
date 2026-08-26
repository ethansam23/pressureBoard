#ifndef LINK_TX_H
#define LINK_TX_H

#include "types.h"
#include "app_config.h"   /* APP_ENABLE_SIM */

/*******************************************************************************
 * Downhole link — UART2 hardware shim, TX owner arbiter, and NVM/refresh fence.
 *
 * OWNS UART2 (TX P1.0 @ 9600 8N1 — the wire to the battery/logger). The debug
 * console (uart_cmd, compiled in only when LINK_CONSOLE_EN) shares the same
 * UART under strict MUTUAL EXCLUSION:
 *
 *   PACKET MODE  (default; the only mode in production builds)
 *     - the ISR serves packet bursts exclusively; console TX is impossible.
 *   BENCH CONSOLE MODE (debug builds, explicit CONSOLE UNLOCK)
 *     - the packet engine suspends AFTER the in-flight packet completes
 *       atomically; the console owns the line at full rate. Re-lock (command,
 *       inactivity timeout, or power cycle) drains the console ring, waits
 *       out a full inter-packet idle, then resumes the stream — a single
 *       packet, never a catch-up burst.
 *
 * The line therefore carries either packets or text — never interleaved.
 *
 * Fence: any operation that can stall the CPU or mask IRQs for milliseconds
 * (NVM flash ops, the acquisition refresh, RAW/SCAN bursts) MUST call
 * link_tx_fence_bounded() first and only proceed on success (fail-closed for
 * NVM; refresh may force-run after repeated deferrals — see main.c). The
 * fence returns true only when no packet is committed and the UART is
 * physically idle, so the stall can never tear a packet on the wire.
 ******************************************************************************/

void link_tx_init(void);        /* UART2 base setup; stream alive immediately
                                 * (NO_READING) — call BEFORE uart_cmd_init   */
void link_tx_service(void);     /* every main-loop iteration                  */

/* Live value (latched per packet at sync start by the state machine) */
void   link_tx_set_live_code(uint16 code);
uint16 link_tx_get_live_code(void);
uint16 link_tx_get_last_code(void);    /* code of the last STARTED packet     */

/* Bench override: RAM-only, auto-expires (LINKTEST_EXPIRY_MS), wins over the
 * live code INCLUDING faults — it tests the link itself. Loudly indicated in
 * STATUS. NOTE: packets are suspended while the console is unlocked, so the
 * forced code appears on the wire after CONSOLE LOCK resumes the stream.    */
void   link_tx_set_test(uint16 code);
void   link_tx_clear_test(void);
bool   link_tx_is_test(void);

#if APP_ENABLE_SIM
/* ---- Bench simulation source (BENCH BUILDS ONLY) -------------------------
 * Runtime state for the synthetic pressure profile. It lives here, beside the
 * LINKTEST override, because it is the same category of thing: a bench-only
 * substitution for what would otherwise reach the wire. The profile maths
 * itself is pure and lives in link_frame.c.
 *
 * RAM-only and NEVER persisted — a reset always returns the board to real
 * acquisition, which also makes a mid-soak reset obvious in the capture.
 * Unlike LINKTEST there is no auto-expiry: a 24-hour soak has to keep
 * running, and the console is locked throughout anyway.                     */
#define SIM_MODE_OFF            0u
#define SIM_MODE_BAR            1u   /* inject the wire code directly        */
#define SIM_MODE_COUNTS         2u   /* inject ADC counts; real cal math runs */

void    link_tx_sim_set(uint8 mode, uint8 phase);
uint8   link_tx_sim_mode(void);
uint8   link_tx_sim_phase(void);
void    link_tx_sim_seek(uint32 index);
uint32  link_tx_sim_index(void);
void    link_tx_sim_advance(void);   /* main.c: once per refresh, after use  */
#endif /* APP_ENABLE_SIM */

/* Fence (fail-closed contract): true = hold acquired, wire idle, safe to
 * stall/mask IRQs; caller MUST link_tx_release() afterwards. false = could
 * not reach a safe window within LINK_FENCE_TIMEOUT_MS; the hold is NOT
 * left set; the caller must NOT stall (skip the NVM write / defer the
 * refresh). link_tx_release() itself transmits nothing — the next packet
 * starts on a later service pass per the overdue policy (single packet,
 * period rebased on its actual sync start).                                 */
bool link_tx_fence_bounded(void);
void link_tx_release(void);

/* Console/packet mutual exclusion (driven by uart_cmd on UNLOCK/LOCK) */
void link_tx_request_console(void);
void link_tx_request_packet(void);
bool link_tx_console_active(void);     /* console owns the line NOW           */

/* Counters for STATUS */
uint32 link_tx_get_pkt_count(void);
uint16 link_tx_get_abort_total(void);
uint16 link_tx_get_busy_skips(void);

/* Called by uart_putc after enqueueing a console byte (debug builds):
 * primes the transmitter if the console owns the line and it is idle.      */
void link_tx_console_kick(void);

/* The single UART2 TX-complete (TI) handler body — uart_cmd_tx_isr (the
 * name wired in the vendor isr_defines.h, which stays untouched) delegates
 * here unconditionally.                                                     */
void link_tx_tx_isr(void);

#endif /* LINK_TX_H */
