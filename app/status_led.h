#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "types.h"

/*******************************************************************************
 * Status LED driver (P0.4, push-pull output).
 *
 * Patterns per PRD §4.7 / design-notes §Status LED:
 *   HEARTBEAT      — short flash every ~3 s (alive)
 *   CAL_ARMED      — 1 Hz blink (waiting for stable capture)
 *   CAL_CAPTURING  — 5 Hz blink (hold at reference pressure)
 *   CAL_STORED     — solid 2 s, then auto-return to heartbeat
 *   FAULT          — double-blink repeating
 ******************************************************************************/

typedef enum
{
    LED_STATE_HEARTBEAT = 0,
    LED_STATE_CAL_ARMED,
    LED_STATE_CAL_CAPTURING,
    LED_STATE_CAL_STORED,
    LED_STATE_FAULT
} led_state_t;

void status_led_init(void);
void status_led_service(void);            /* call every main-loop iteration   */
void status_led_set_state(led_state_t s); /* transition to a new LED state    */
led_state_t status_led_get_state(void);   /* current state (for the arbiter)  */

#endif /* STATUS_LED_H */
