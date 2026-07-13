#ifndef OUTPUT_H
#define OUTPUT_H

#include "types.h"

/*******************************************************************************
 * PWM-DAC output stage — CCU6 T12 channel 2 on P0.1 (CC62_0).
 *
 * Maps ADC counts (0-1023) into the 0.5-4.5 V sub-range.
 * Fault bands sit outside that range (<=0.25 V, >=4.75 V).
 * ~19.5 kHz edge-aligned PWM, 10-bit resolution.
 ******************************************************************************/

void   output_init(void);
void   output_set_pressure(uint16 counts);  /* 0-1023 ADC → 0.5-4.5 V     */
void   output_set_fault_low(void);          /* drive <= 0.25 V             */
void   output_set_fault_high(void);         /* drive >= 4.75 V             */

/* Calibrated pressure output (bar → voltage), mapped over the runtime window */
void   output_set_pressure_bar(float bar);  /* range_lo→0.5V, range_hi→4.5V */

/* UART manual override for bench testing */
void   output_set_manual(uint16 counts);    /* override with raw counts    */
void   output_set_auto(void);              /* return to live tracking     */

/* Introspection for the bench UI */
uint16 output_get_duty(void);               /* last commanded PWM duty      */
bool   output_is_manual(void);              /* manual override active?      */

#endif /* OUTPUT_H */
