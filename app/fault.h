#ifndef FAULT_H
#define FAULT_H

#include "types.h"

/*******************************************************************************
 * Fault flags driving the out-of-band analog signaling.
 *
 * Two independent sources, OR-ed by fault_is_active():
 *  - probe disagreement: |probe_a - probe_b| > threshold, auto-clears when
 *    the probes agree again (fault_check / fault_clear).
 *  - system fault: excitation (VDDEXT) unstable, raised/cleared by main's
 *    supervision (fault_raise_system / fault_clear_system).
 *
 * LED indication is arbitrated centrally in main — this module only tracks
 * the flags.
 ******************************************************************************/

void fault_init(void);
void fault_check(uint16 probe_a, uint16 probe_b);
bool fault_is_active(void);
void fault_clear(void);          /* clear disagreement (e.g. single-probe)    */

void fault_raise_system(void);   /* excitation/system fault                   */
void fault_clear_system(void);

#endif /* FAULT_H */
