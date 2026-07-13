#ifndef UART_CMD_H
#define UART_CMD_H

#include "types.h"

/*******************************************************************************
 * UART command interface — bench only (TX P1.0, RX P1.1).
 *
 * ISR-driven ring-buffer TX and RX.  Strictly non-blocking — safe for WDT1.
 * Line-based command protocol terminated by \r or \n.
 *
 * Commands: STATUS, RAW, SCAN, AUTO, RATE, THRESH, RANGE, PROBE, OUTPUT,
 *           POWER, CAL, HELP
 ******************************************************************************/

void uart_cmd_init(void);
void uart_cmd_service(void);   /* call every main-loop iteration             */

/* Called by main.c after each acquisition_run(). */
void uart_cmd_update_readings(uint16 probe_a, uint16 probe_b, uint16 combined);

/* Non-blocking string send (for use by other modules if needed). */
void uart_send_str(const char *s);
void uart_send_u16(uint16 val);
void uart_send_i32(sint32 val);   /* signed, |val| <= 65535 (BootROM rc)   */
void uart_send_hex16(uint16 val); /* fixed-width 4-digit hex, e.g. "00A4" */
void uart_send_float3(float f);   /* 3 decimal places, e.g. "14.700" */

/* Bounded TX drain (~max 200 ms, WDT-serviced). ONLY for getting a
 * diagnostic line out before masking interrupts for a flash op — normal
 * code must stay non-blocking (PRD §4.4). */
void uart_tx_flush_bounded(void);

/* ISR callbacks — called from UART2_IRQHandler via isr_defines.h macros.     */
void uart_cmd_rx_isr(void);
void uart_cmd_tx_isr(void);

#endif /* UART_CMD_H */
