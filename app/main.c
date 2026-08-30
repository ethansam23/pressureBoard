#include "tle_device.h"
#include "app_config.h"
#include "scheduler.h"
#include "status_led.h"
#include "acquisition.h"
#include "link_frame.h"
#include "link_tx.h"
#include "calibration.h"
#include "fault.h"
#include "nvm_config.h"
#include "uart_cmd.h"
#include "spi_nor.h"

static acq_result_t acq;

/* VDDEXT_CTRL.STABLE without the enable+200 µs delay of PMU_VDDEXT_On() —
 * used for the per-refresh excitation supervision. */
static bool vddext_stable(void)
{
    return (u1_Field_Rd32(&PMU->VDDEXT_CTRL.reg,
                          (uint8)PMU_VDDEXT_CTRL_VDDEXT_STABLE_Pos,
                          PMU_VDDEXT_CTRL_VDDEXT_STABLE_Msk) == 1u);
}

/* Capture, then clear, the regulator's latched shutdown cause. Returns true
 * if anything was latched.
 *
 * This is the difference between a rail that recovers and one that is dead
 * until the next power cycle: VDDEXT latches its UV/OT cause, and
 * PMU_VDDEXT_On() only re-asserts ENABLE. With the latch standing, every
 * retry is a no-op and the rail sits at 0 V forever.
 *
 * Capture before clearing — clearing destroys the only record of WHY the
 * rail went down, and on a sealed board that record is the whole diagnosis. */
static bool vddext_clear_latch(void)
{
    uint8 cause = 0u;

    if (u1_Field_Rd32(&PMU->VDDEXT_CTRL.reg,
                      (uint8)PMU_VDDEXT_CTRL_VDDEXT_UV_IS_Pos,
                      PMU_VDDEXT_CTRL_VDDEXT_UV_IS_Msk) == 1u)
    {
        cause |= (uint8)VDDEXT_CAUSE_UV;
    }
    if (u1_Field_Rd32(&PMU->VDDEXT_CTRL.reg,
                      (uint8)PMU_VDDEXT_CTRL_VDDEXT_OT_IS_Pos,
                      PMU_VDDEXT_CTRL_VDDEXT_OT_IS_Msk) == 1u)
    {
        cause |= (uint8)VDDEXT_CAUSE_OT;
    }

    if (cause == 0u) { return false; }

    fault_note_vddext_cause(cause);

    if ((cause & (uint8)VDDEXT_CAUSE_UV) != 0u)
    {
        PMU_VDDEXT_UV_Int_Clr();                 /* VDDEXT_UV_ISC            */
    }
    if ((cause & (uint8)VDDEXT_CAUSE_OT) != 0u)
    {
        PMU_VDDEXT_OT_Int_Clr();                 /* VDDEXT_OT_ISC            */
        PMU_VDDEXT_OT_Clr();                     /* VDDEXT_OT_SC             */
    }
    return true;
}

/* Full re-enable attempt for a rail that is NOT currently stable. Clearing
 * the latch is what lets the regulator restart; the ENABLE 0->1 edge is
 * deliberate belt-and-braces — the datasheet documents that undervoltage
 * SETS VDDEXT_UV_IS (P_2.3.9) but not whether clearing alone restarts the
 * LDO, so this works under either semantics.
 *
 * Only toggled when something was actually latched: a rail that is merely
 * still ramping into its output cap must be left alone to finish, not
 * restarted. Called at most once per refresh (>=100 ms), so a hard overload
 * gets retried at a sane cadence rather than cycled.                        */
static bool vddext_recover(void)
{
    if (vddext_clear_latch())
    {
        (void)PMU_VDDEXT_Off();
    }
    return PMU_VDDEXT_On();
}

/* Single owner of the LED pattern. Modules no longer set the LED directly
 * (they used to overwrite each other); the one exception is the transient
 * CAL_STORED "solid 2 s" pushed by calibration_store(), which is allowed to
 * finish before arbitration resumes.
 * Priority: fault > cal capturing > cal armed > heartbeat. */
static void led_arbitrate(void)
{
    led_state_t cur = status_led_get_state();
    led_state_t desired;
    cal_state_t cs = calibration_get_state();

    if (cur == LED_STATE_CAL_STORED) { return; }

    if (fault_is_active())        { desired = LED_STATE_FAULT; }
    else if (cs == CAL_CAPTURING) { desired = LED_STATE_CAL_CAPTURING; }
    else if (cs == CAL_ARMED)     { desired = LED_STATE_CAL_ARMED; }
    else                          { desired = LED_STATE_HEARTBEAT; }

    if (desired != cur) { status_led_set_state(desired); }
}

int main(void)
{
    /* ---- SDK init --------------------------------------------------------- *
     * The startup code already ran SystemInit() (system_tle985x.c), which
     * issued the FIRST WDT1 trigger — ending the 200 ms power-up long open
     * window — and started SysTick. Do NOT call WDT1_Init() or trigger WDT1
     * here: a second trigger a few ms after the first lands in the CLOSED
     * window -> reset, and 5 such resets latch the chip into Sleep Mode.
     * (Invisible under J-Link — WDT1 is disabled in Debug Mode.) The next
     * service happens in scheduler_service() once the window opens
     * (WD_Counter > 699 ms, counted by WDT1_Window_Count() in
     * scheduler_tick()).                                                     */
    TLE_Init();

    /* ---- Application module init ---------------------------------------- */
    scheduler_init();
    nvm_config_init();
    scheduler_set_rate_ms(nvm_config_get_rate_ms());

    status_led_init();
    acquisition_init();
    link_tx_init();             /* owns UART2; the packet stream is alive from
                                 * here, carrying NO_READING (boot fail-safe:
                                 * the wire never shows a fake pressure)      */
    calibration_init();
    fault_init();
    uart_cmd_init();            /* debug builds: console RX, BOOTS LOCKED —
                                 * zero TX text until CONSOLE UNLOCK          */

    /* Onboard NOR flash (SSC1 — never touches UART2, so no fence is needed
     * and the stream started above is undisturbed). The probe is a 4-byte
     * blocking transaction, ~32 µs; everything downstream gates on its
     * result, so a missing or miswired flash costs one ID read and is then
     * reported through the bench console rather than retried on the wire.  */
    spi_nor_init();
    (void)spi_nor_probe();

    /* Reset cause: captured for the console unlock banner (nothing may
     * print at boot — the wire carries packets only), then cleared so the
     * next boot reports fresh.                                              */
    uart_cmd_set_boot_info(PMU->RESET_STS.reg, PMU->WFS.reg);
    PMU->RESET_STS.reg = 0u;

    /* ---- Enable VDDEXT; BOUNDED wait for it to stabilize ---------------- *
     * Never spin here forever. VDDEXT can latch off (undervoltage/overtemp),
     * and a warm NRST may not clear that latch -> an unbounded wait would
     * hang on every reset with the rail down. Time-box it; on timeout latch
     * the VDDEXT fault so the wire carries its code — LED and console are
     * bench-only and invisible downhole. The link is serviced THROUGHOUT
     * the wait so the stream starts on schedule.                            */
    {
        uint32 vddext_t0;

        /* Clear a latch carried in from before this reset FIRST. A warm NRST
         * does not necessarily clear it, and while it stands the wait below
         * is 100 ms of no-op retries ending in a fault every single boot.
         * Cleared once, outside the loop: re-clearing per iteration would
         * restart the ramp every 200 µs and a large output cap would never
         * finish charging.                                                  */
        (void)vddext_clear_latch();

        vddext_t0 = scheduler_get_ms();
        while (PMU_VDDEXT_On() == false)
        {
            (void)WDT1_Service();
            link_tx_service();
            if ((scheduler_get_ms() - vddext_t0) >= VDDEXT_SETTLE_TIMEOUT_MS)
            {
                fault_raise_vddext();
                break;   /* proceed to the main loop anyway */
            }
        }
    }

    /* ================================================================== */
    /*  Super-loop — cooperative, non-blocking, run-to-completion tasks   */
    /* ================================================================== */
    for (;;)
    {
        /* Refresh deferral state: the scheduler flag is consume-once, so it
         * is latched here and only cleared when the refresh actually runs. */
        static bool  refresh_due;
        static uint8 refresh_defers;

        /* — Tick / WDT1 ------------------------------------------------ */
        (void)scheduler_service();

        /* — Periodic pipeline (sample → fault → cal → link code) -------- *
         * FENCED: acquisition can stall ~34 ms when the ADC is dead — the
         * exact condition whose fault code the wire must carry — so the
         * packet engine is brought to a wire-idle hold first. Predictable
         * stalls POSTPONE packets; they never tear one. If the fence can't
         * be acquired (wedged UART), the refresh defers and retries, with
         * a 3-strike escape so acquisition is never starved. In bench
         * console mode the stream is suspended and no fence is needed.     */
        if (scheduler_refresh_pending()) { refresh_due = true; }
        if (refresh_due)
        {
            bool fenced = false;
            bool safe   = link_tx_console_active();
            if (!safe)
            {
                fenced = link_tx_fence_bounded();
                safe   = fenced || (refresh_defers >= 3u);
            }

            if (safe)
            {
                uint16 code;

                refresh_due    = false;
                refresh_defers = 0u;

                acquisition_run(&acq);

                /* System supervision: a dead/stalled ADC or a sagging
                 * VDDEXT both make readings untrustworthy while the probes
                 * still AGREE (the disagreement check can't catch either).
                 * Tracked per-cause so the wire reports WHICH one. On
                 * VDDEXT instability, attempt a re-enable — recovers a
                 * latched-off regulator, not just a sag.                   */
                if (acq.stalled) { fault_raise_adc(); }
                else             { fault_clear_adc(); }

                if (vddext_stable() || vddext_recover())
                {
                    fault_clear_vddext();
                }
                else
                {
                    fault_raise_vddext();
                }

                /* Fault check — disagreement only means something with two
                 * probes.                                                  */
                if (nvm_config_get_probe_mode() == PROBE_MODE_DUAL)
                {
                    fault_check(acq.probe_a, acq.probe_b);
                }
                else
                {
                    fault_clear();   /* single probe: clear latched fault  */
                }

                /* Calibration capture accumulator — pause while a fault is
                 * active so a corrupted reading can't average into a point */
                if (!fault_is_active())
                {
                    cal_state_t before = calibration_get_state();
                    calibration_service(acq.combined);
                    if (before == CAL_CAPTURING &&
                        calibration_get_state() == CAL_ARMED)
                    {
                        uart_send_str("Captured (");
                        uart_send_u16(calibration_get_num_points());
                        uart_send_str(" pts)\r\n");
                    }
                }

                /* Link code — priority ladder (one code per packet; the
                 * bench console can list all causes). Uncalibrated sends a
                 * status code, NEVER raw counts dressed up as pressure.    */
                if      (fault_adc_active())      { code = (uint16)LINK_CODE_ADC_STALL; }
                else if (fault_vddext_active())   { code = (uint16)LINK_CODE_VDDEXT; }
                else if (fault_disagree_active()) { code = (uint16)LINK_CODE_DISAGREE; }
                else if (calibration_is_valid())
                {
                    code = link_encode_bar(calibration_apply(acq.combined));
                }
                else                              { code = (uint16)LINK_CODE_UNCAL; }
                link_tx_set_live_code(code);

                if (fenced) { link_tx_release(); }

                /* AUTO stream last, so the line reflects THIS cycle's
                 * state (debug console, unlocked sessions only).           */
                uart_cmd_update_readings(acq.probe_a, acq.probe_b, acq.combined);
            }
            else
            {
                refresh_defers++;
            }
        }

        /* — Cooperative background tasks -------------------------------- *
         * Link first: it owns the tightest deadline on the wire.          */
        link_tx_service();
        led_arbitrate();
        status_led_service();
        uart_cmd_service();
    }
}
