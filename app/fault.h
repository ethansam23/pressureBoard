#ifndef FAULT_H
#define FAULT_H

#include "types.h"

/*******************************************************************************
 * Fault flags driving the out-of-band output signaling.
 *
 * Three independent causes, OR-ed by fault_is_active():
 *  - probe disagreement: |probe_a - probe_b| > threshold, auto-clears when
 *    the probes agree again (fault_check / fault_clear).
 *  - ADC stall: conversions timing out, raised/cleared by main's supervision
 *    from acq.stalled.
 *  - VDDEXT: excitation rail unstable, raised/cleared by main's supervision.
 *
 * The causes are exposed individually so the output stage can report WHICH
 * fault is active (the downhole link carries a distinct code per cause).
 *
 * LED indication is arbitrated centrally in main — this module only tracks
 * the flags.
 ******************************************************************************/

void fault_init(void);
void fault_check(uint16 probe_a, uint16 probe_b);
bool fault_is_active(void);          /* OR of all causes                       */
void fault_clear(void);              /* clear disagreement (e.g. single-probe) */

void fault_raise_adc(void);          /* ADC stalled / conversions dead         */
void fault_clear_adc(void);
void fault_raise_vddext(void);       /* excitation rail unstable               */
void fault_clear_vddext(void);

/* ---- VDDEXT hardware shutdown cause ------------------------------------ *
 * The regulator LATCHES why it shut down (VDDEXT_CTRL.VDDEXT_UV_IS /
 * VDDEXT_OT_IS) and will not restart while the latch stands. Main's recovery
 * path must clear it — which destroys the only record of the cause. So it
 * captures the bits here first: sticky since boot, reported by the bench
 * console, so a rail that died and recovered is still diagnosable.          */
#define VDDEXT_CAUSE_UV  0x01u       /* undervoltage shutdown (VDDEXT_UV_IS)   */
#define VDDEXT_CAUSE_OT  0x02u       /* overtemperature       (VDDEXT_OT_IS)   */

void   fault_note_vddext_cause(uint8 cause);  /* OR in + count the clear      */
uint8  fault_get_vddext_cause(void);          /* sticky OR since boot         */
uint16 fault_get_vddext_trips(void);          /* latched shutdowns cleared    */

/* Per-cause queries (priority/encoding decided by the caller) */
bool fault_disagree_active(void);
bool fault_adc_active(void);
bool fault_vddext_active(void);

#endif /* FAULT_H */
