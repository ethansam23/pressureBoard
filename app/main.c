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

static acq_result_t acq;

/* VDDEXT_CTRL.STABLE without the enable+200 µs delay of PMU_VDDEXT_On() —
 * used for the per-refresh excitation supervision. */
static bool vddext_stable(void)
{
    return (u1_Field_Rd32(&PMU->VDDEXT_CTRL.reg,
                          (uint8)PMU_VDDEXT_CTRL_VDDEXT_STABLE_Pos,
                          PMU_VDDEXT_CTRL_VDDEXT_STABLE_Msk) == 1u);
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

#if APP_ENABLE_SIM
/* Bench simulation source. Substitutes the transducers so the link path can
 * be verified in isolation. Returns the profile's wire code for this refresh
 * and advances the index.
 *
 * SIM_MODE_COUNTS back-solves the calibration so the wire follows the same
 * recognisable profile while the REAL cal + encode maths runs on the way out.
 * It is deliberately NOT an exact reproduction and must not be verified as
 * one: counts are 12-bit-scaled over the configured window, so one count is
 * ~2.4 dbar at a 0-1000 bar span — far coarser than the 0.1 dbar wire LSB.
 * The profile's 1 dbar steps therefore quantise into a staircase. The host
 * verifier scores COUNTS runs against a counts-quantised expectation with
 * that tolerance; only SIM_MODE_BAR is checked value-exact.                */
static uint16 sim_step_code(acq_result_t *a)
{
    uint32 idx;
    uint8  phase;
    uint16 code;

    /* Start beacon (autostart only): full scale for a fixed spell before the
     * profile begins, so the start of a run is unmissable in the logger's own
     * dump. It does NOT advance the profile index — index 0 must still be the
     * profile's first sample, so the reference stream stays unchanged and the
     * host verifier simply skips the leading beacon before aligning.       */
    if (link_tx_sim_beacon_left() != 0u)
    {
        link_tx_sim_beacon_tick();
        return (uint16)SIM_AUTOSTART_BEACON_CODE;
    }

    idx   = link_tx_sim_index();
    phase = link_tx_sim_phase();
    code  = sim_profile_code(idx, scheduler_get_rate_ms(), phase);

    link_tx_sim_advance();

    if (link_tx_sim_mode() == SIM_MODE_COUNTS)
    {
        /* Status codes have no counts preimage — pass them straight through
         * so the fault portion of the profile still exercises the wire.    */
        if (code <= (uint16)LINK_VALUE_MAX)
        {
            float slope = calibration_get_slope();
            if (calibration_is_valid() && (slope != 0.0f))
            {
                float counts = (((float)code / 10.0f) - calibration_get_offset())
                               / slope;
                uint16 c;
                /* Round rather than truncate — halves the quantisation error
                 * that the coarse count scale already forces on us.        */
                counts += 0.5f;
                if (counts < 0.0f)                  { counts = 0.0f; }
                if (counts > (float)ADC_COUNTS_MAX) { counts = (float)ADC_COUNTS_MAX; }
                c = (uint16)counts;
                a->probe_a  = c;
                a->probe_b  = c;
                a->combined = c;
                a->stalled  = false;
            }
            /* No valid calibration: leave acq alone. The ladder below then
             * reports UNCAL, which is the honest answer — do not fabricate. */
        }
        return code;
    }
    return code;   /* SIM_MODE_BAR: injected at the ladder, acq untouched   */
}
#endif /* APP_ENABLE_SIM */

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

    /* Reset cause: captured for the console unlock banner (nothing may
     * print at boot — the wire carries packets only), then cleared so the
     * next boot reports fresh.                                              */
    uart_cmd_set_boot_info(PMU->RESET_STS.reg, PMU->WFS.reg);
    PMU->RESET_STS.reg = 0u;

#if APP_SIM_AUTOSTART
    /* Arm the synthetic profile at boot so an unattended soak needs no host
     * at all — power on, or press reset, and the board streams. The logger
     * has only an RX line and can arm nothing, and unlocking the console to
     * arm by hand would suspend the very stream under test.
     *
     * Beacon length is resolved against the rate loaded from NVM just above,
     * so it is a fixed wall-clock 30 s whatever RATE is set to.            */
    link_tx_sim_autostart((uint8)SIM_AUTOSTART_MODE, (uint8)SIM_AUTOSTART_PHASE,
                          SIM_AUTOSTART_BEACON_MS / scheduler_get_rate_ms());
#endif

    /* ---- Enable VDDEXT; BOUNDED wait for it to stabilize ---------------- *
     * Never spin here forever. VDDEXT can latch off (undervoltage/overtemp),
     * and a warm NRST may not clear that latch -> an unbounded wait would
     * hang on every reset with the rail down. Time-box it; on timeout latch
     * the VDDEXT fault so the wire carries its code — LED and console are
     * bench-only and invisible downhole. The link is serviced THROUGHOUT
     * the wait so the stream starts on schedule.                            */
    {
        uint32 vddext_t0 = scheduler_get_ms();
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
                bool   laddered = true;   /* false = sim bypassed the ladder */

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

                if (vddext_stable() || PMU_VDDEXT_On())
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
#if APP_ENABLE_SIM
                /* Bench sim runs BEFORE the ladder so SIM_MODE_COUNTS can
                 * seed acq and still be scored by the real fault/cal path.
                 * SIM_MODE_BAR bypasses the ladder — that is the point of it
                 * (pure link-path isolation) — but genuine ADC and excitation
                 * faults still win, so a rig problem can never be masked by
                 * synthetic data.                                          */
                if (link_tx_sim_mode() != SIM_MODE_OFF)
                {
                    uint16 sim_code = sim_step_code(&acq);
                    if (link_tx_sim_mode() == SIM_MODE_BAR)
                    {
                        if      (fault_adc_active())    { sim_code = (uint16)LINK_CODE_ADC_STALL; }
                        else if (fault_vddext_active()) { sim_code = (uint16)LINK_CODE_VDDEXT; }
                        code      = sim_code;
                        laddered  = false;
                    }
                }
#endif
                if (laddered)
                {
                    if      (fault_adc_active())      { code = (uint16)LINK_CODE_ADC_STALL; }
                    else if (fault_vddext_active())   { code = (uint16)LINK_CODE_VDDEXT; }
                    else if (fault_disagree_active()) { code = (uint16)LINK_CODE_DISAGREE; }
                    else if (calibration_is_valid())
                    {
                        code = link_encode_bar(calibration_apply(acq.combined));
                    }
                    else                              { code = (uint16)LINK_CODE_UNCAL; }
                }
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
