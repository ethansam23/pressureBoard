#ifndef SCHEDULER_H
#define SCHEDULER_H

#include "types.h"

/*******************************************************************************
 * Time-triggered scheduler + WDT1 service + operating-mode abstraction.
 *
 * SysTick runs at 1 kHz (1 ms).  scheduler_tick() is called from the
 * SysTick ISR callback to keep the monotonic counter ticking.
 * scheduler_service() is called from the main super-loop; it services
 * WDT1 and evaluates whether a new refresh interval has elapsed.
 ******************************************************************************/

void   scheduler_init(void);

/* Called from SysTick ISR — keep tiny and non-blocking. */
void   scheduler_tick(void);

/* Call every main-loop iteration.  Services WDT1, checks refresh tick.
 * Returns true when a WDT1 service actually triggered this pass.           */
bool   scheduler_service(void);

/* Returns true once per refresh interval, then auto-clears. */
bool   scheduler_refresh_pending(void);

/* Monotonic millisecond counter (wraps at ~49.7 days). */
uint32 scheduler_get_ms(void);

/* Runtime refresh-rate change (clamped to min/max). */
void   scheduler_set_rate_ms(uint32 ms);
uint32 scheduler_get_rate_ms(void);

/* Operating mode (v2 seam — always MODE_CONTINUOUS for now). */
uint8  scheduler_get_mode(void);

#endif /* SCHEDULER_H */
