#include "status_led.h"
#include "app_config.h"
#include "scheduler.h"
#include "port.h"

static led_state_t state;
static uint32      last_ms;
static uint8       phase;       /* sub-step within a pattern                 */
static bool        led_on;

/* ---- helpers ------------------------------------------------------------ */
static void led_set(bool on)
{
    if (on) { PORT_P04_Output_High_Set(); }
    else    { PORT_P04_Output_Low_Set();  }
    led_on = on;
}

/* ---- public ------------------------------------------------------------- */
void status_led_init(void)
{
    PORT_P04_Output_Set();          /* configure P0.4 as push-pull output    */
    led_set(false);
    state   = LED_STATE_HEARTBEAT;
    last_ms = 0u;
    phase   = 0u;
}

void status_led_set_state(led_state_t s)
{
    state   = s;
    phase   = 0u;
    last_ms = scheduler_get_ms();
    led_set(false);
}

led_state_t status_led_get_state(void)
{
    return state;
}

void status_led_service(void)
{
    uint32 now     = scheduler_get_ms();
    uint32 elapsed = now - last_ms;

    switch (state)
    {
    /* ---- Heartbeat: short flash every ~3 s ----------------------------- */
    case LED_STATE_HEARTBEAT:
    {
        uint32 interval = led_on ? LED_HEARTBEAT_ON_MS : LED_HEARTBEAT_OFF_MS;
        if (elapsed >= interval)
        {
            led_set(!led_on);
            last_ms = now;
        }
        break;
    }

    /* ---- Cal armed: symmetric 1 Hz blink ------------------------------- */
    case LED_STATE_CAL_ARMED:
        if (elapsed >= LED_CAL_ARMED_MS)
        {
            led_set(!led_on);
            last_ms = now;
        }
        break;

    /* ---- Cal capturing: 5 Hz blink ------------------------------------- */
    case LED_STATE_CAL_CAPTURING:
        if (elapsed >= LED_CAL_CAPTURE_MS)
        {
            led_set(!led_on);
            last_ms = now;
        }
        break;

    /* ---- Cal stored: solid 2 s then back to heartbeat ------------------ */
    case LED_STATE_CAL_STORED:
        if (!led_on)
        {
            led_set(true);
            last_ms = now;
        }
        else if (elapsed >= LED_CAL_STORED_MS)
        {
            status_led_set_state(LED_STATE_HEARTBEAT);
        }
        break;

    /* ---- Fault: double-blink pattern ----------------------------------- */
    /*  phase 0: ON  (LED_FAULT_ON_MS)                                      */
    /*  phase 1: OFF (LED_FAULT_GAP_MS)                                     */
    /*  phase 2: ON  (LED_FAULT_ON_MS)                                      */
    /*  phase 3: OFF (LED_FAULT_PAUSE_MS) → repeat                          */
    case LED_STATE_FAULT:
    {
        uint32 dur;
        switch (phase)
        {
        case 0u: dur = LED_FAULT_ON_MS;    break;
        case 1u: dur = LED_FAULT_GAP_MS;   break;
        case 2u: dur = LED_FAULT_ON_MS;    break;
        default: dur = LED_FAULT_PAUSE_MS; break;
        }
        if (elapsed >= dur)
        {
            phase = (phase + 1u) & 0x03u;          /* 0→1→2→3→0 */
            led_set((phase == 0u) || (phase == 2u));
            last_ms = now;
        }
        break;
    }

    default:
        break;
    }
}
