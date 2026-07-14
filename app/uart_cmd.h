#ifndef UART_CMD_H
#define UART_CMD_H

#include "types.h"

/*******************************************************************************
 * Bench console — debug builds only (LINK_CONSOLE_EN), sharing UART2 with the
 * downhole packet stream under strict mutual exclusion (see link_tx.h).
 *
 * Boots LOCKED (zero TX text, commands ignored) until the exact line
 * "CONSOLE UNLOCK" arrives; unlocking suspends the packet stream. Re-locks on
 * "CONSOLE LOCK", 5 minutes of RX inactivity, or power cycle.
 *
 * Commands: STATUS, RAW, SCAN, AUTO, RATE, THRESH, PROBE, LINKTEST,
 *           CONSOLE LOCK/UNLOCK, POWER, CAL, PSI/BAR, HELP
 *
 * In production builds every function here compiles to a no-op (uart_send_*
 * included), so callers need no guards and the wire stays packet-pure.
 ******************************************************************************/

void uart_cmd_init(void);      /* call AFTER link_tx_init (which owns UART2)  */
void uart_cmd_service(void);   /* call every main-loop iteration              */

/* Reset-cause registers captured by main() at boot, shown in the unlock
 * banner (the console is locked at boot, so nothing prints then).           */
void uart_cmd_set_boot_info(uint32 rst, uint32 wfs);

/* Called by main.c after each acquisition_run(). */
void uart_cmd_update_readings(uint16 probe_a, uint16 probe_b, uint16 combined);

/* Non-blocking string send (no-ops while locked / in production builds). */
void uart_send_str(const char *s);
void uart_send_u16(uint16 val);
void uart_send_u32(uint32 val);
void uart_send_i32(sint32 val);   /* signed, |val| <= 65535 (BootROM rc)   */
void uart_send_hex16(uint16 val); /* fixed-width 4-digit hex, e.g. "00A4" */
void uart_send_float3(float f);   /* 3 decimal places, e.g. "14.700" */

/* Bounded TX drain (WDT-serviced; returns immediately unless the console
 * owns the line). ONLY for getting a diagnostic line out before masking
 * interrupts for a flash op — normal code must stay non-blocking.           */
void uart_tx_flush_bounded(void);

/* ISR callbacks — called from UART2_IRQHandler via isr_defines.h macros.
 * uart_cmd_tx_isr delegates unconditionally to the link TX arbiter.         */
void uart_cmd_rx_isr(void);
void uart_cmd_tx_isr(void);

/* Console-ring accessors for the link arbiter (ISR / kick context).         */
bool uart_cmd_console_pop(uint8 *b);
bool uart_cmd_console_ring_empty(void);

#endif /* UART_CMD_H */
