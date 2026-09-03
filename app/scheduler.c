#include "scheduler.h"
#include "app_config.h"
#include "wdt1.h"

static volatile uint32 tick_ms;         /* incremented in SysTick ISR         */
static volatile bool   refresh_flag;    /* set when a refresh interval passes */

static uint32 refresh_rate_ms;
static uint32 last_refresh_ms;

/* ---- Init --------------------------------------------------------------- */
void scheduler_init(void)
{
    tick_ms         = 0u;
    /* Fire the FIRST refresh on the opening loop pass instead of one full
     * interval after boot. The output boots in the fault-low band and only
     * leaves it once a reading lands, so without this the analog line sits
     * out-of-band for a whole refresh period at every power-up -- 10 s at
     * the default rate, which the battery reads as a fault.               */
    refresh_flag    = true;
    refresh_rate_ms = REFRESH_RATE_DEFAULT_MS;
    last_refresh_ms = 0u;
}

/* ---- ISR context (SysTick callback — 1 ms) ------------------------------ */
void scheduler_tick(void)
{
    tick_ms++;
    /* Do NOT call WDT1_Window_Count() here: the SDK's SysTick_Handler
     * (isr.c) already calls it unconditionally AFTER this callback. A second
     * increment makes WD_Counter run at 2/ms, so WDT1_Service() fires at
     * ~350 ms real time -- inside the hardware CLOSED window -> reset loop
     * on every standalone boot (invisible under J-Link). */
}

/* ---- Main-loop context -------------------------------------------------- */
bool scheduler_service(void)
{
    /* Always service WDT1 when the window is open. Returns whether a
     * service actually triggered this pass (used by boot diagnostics).     */
    bool serviced = WDT1_Service();

    /* Check whether a new refresh interval has elapsed. */
    uint32 now = tick_ms;
    if ((now - last_refresh_ms) >= refresh_rate_ms)
    {
        refresh_flag    = true;
        last_refresh_ms = now;
    }
    return serviced;
}

bool scheduler_refresh_pending(void)
{
    if (refresh_flag)
    {
        refresh_flag = false;
        return true;
    }
    return false;
}

/* ---- Accessors ---------------------------------------------------------- */
uint32 scheduler_get_ms(void)
{
    return tick_ms;
}

void scheduler_set_rate_ms(uint32 ms)
{
    if (ms < REFRESH_RATE_MIN_MS)
    {
        ms = REFRESH_RATE_MIN_MS;
    }
    else if (ms > REFRESH_RATE_MAX_MS)
    {
        ms = REFRESH_RATE_MAX_MS;
    }
    refresh_rate_ms = ms;
}

uint32 scheduler_get_rate_ms(void)
{
    return refresh_rate_ms;
}

uint8 scheduler_get_mode(void)
{
    return (uint8)MODE_CONTINUOUS;
}
