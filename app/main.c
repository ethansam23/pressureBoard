#include "tle_device.h"
#include "app_config.h"
#include "scheduler.h"
#include "status_led.h"
#include "acquisition.h"
#include "output.h"
#include "calibration.h"
#include "fault.h"
#include "nvm_config.h"
#include "uart_cmd.h"

static acq_result_t acq;

/* ===== TEMP standalone bring-up diagnostics — remove once the power-cycle
 * reset loop is solved. Prints the hardware reset cause + timestamped
 * progress markers so a UART capture shows where each boot dies.           */
static void diag_print_reset_cause(void)
{
    uint32 rst = PMU->RESET_STS.reg;
    uint32 wfs = PMU->WFS.reg;

    uart_send_str("RST 0x");
    uart_send_hex16((uint16)rst);
    uart_send_str(" WFS 0x");
    uart_send_hex16((uint16)wfs);
    uart_send_str(" :");
    if ((rst & PMU_RESET_STS_PMU_VS_POR_Msk) != 0u) { uart_send_str(" POR");     }
    if ((rst & PMU_RESET_STS_PMU_PIN_Msk)    != 0u) { uart_send_str(" PIN");     }
    if ((rst & PMU_RESET_STS_PMU_ExtWDT_Msk) != 0u) { uart_send_str(" WDT1");    }
    if ((rst & PMU_RESET_STS_PMU_IntWDT_Msk) != 0u) { uart_send_str(" IntWDT");  }
    if ((rst & PMU_RESET_STS_PMU_ClkWDT_Msk) != 0u) { uart_send_str(" ClkWDT");  }
    if ((rst & PMU_RESET_STS_PMU_SOFT_Msk)   != 0u) { uart_send_str(" SOFT");    }
    if ((rst & PMU_RESET_STS_LOCKUP_Msk)     != 0u) { uart_send_str(" LOCKUP");  }
    if ((rst & PMU_RESET_STS_SYS_FAIL_Msk)   != 0u) { uart_send_str(" SYSFAIL"); }
    if ((rst & PMU_RESET_STS_PMU_LPR_Msk)    != 0u) { uart_send_str(" LPR");     }
    if ((rst & (PMU_RESET_STS_PMU_WAKE_Msk |
                PMU_RESET_STS_PMU_SleepEX_Msk)) != 0u) { uart_send_str(" WAKE"); }
    if ((wfs & PMU_WFS_WDT1_SEQ_FAIL_Msk)    != 0u) { uart_send_str(" [WDT1_SEQ_FAIL]"); }
    uart_send_str("\r\n");

    PMU->RESET_STS.reg = 0u;   /* clear-if-RW so each boot reports fresh     */
}

static void diag_mark(const char *tag)
{
    uart_send_str(tag);
    uart_send_str(" t=");
    uart_send_u16((uint16)scheduler_get_ms());
    uart_send_str("\r\n");
}
/* ===== end TEMP diagnostics ============================================== */

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
    output_init();              /* boots in the fault-low band (fail-safe)    */
    calibration_init();
    fault_init();
    uart_cmd_init();

    diag_print_reset_cause();          /* TEMP bring-up diagnostic           */

    if (!nvm_flash_is_healthy())
    {
        uart_send_str("WARN: NVM data flash inconsistent (saves disabled)\r\n");
    }

    /* ---- Enable VDDEXT; BOUNDED wait for it to stabilize ---------------- *
     * Never spin here forever. VDDEXT can latch off (undervoltage/overtemp),
     * and a warm NRST may not clear that latch -> an unbounded wait would
     * hang on every reset with the rail down. Time-box it; on timeout latch
     * a system fault so the analog line signals out-of-band (fault-low) —
     * UART and LED are bench-only and invisible downhole.                   */
    {
        uint32 vddext_t0 = scheduler_get_ms();
        while (PMU_VDDEXT_On() == false)
        {
            (void)WDT1_Service();
            if ((scheduler_get_ms() - vddext_t0) >= VDDEXT_SETTLE_TIMEOUT_MS)
            {
                uart_send_str("WARN: VDDEXT not stable (excitation down)\r\n");
                fault_raise_vddext();
                break;   /* proceed to the command loop anyway */
            }
        }
    }
    diag_mark("vddext");               /* TEMP bring-up diagnostic           */

    diag_mark("loop");                 /* TEMP bring-up diagnostic           */

    /* ================================================================== */
    /*  Super-loop — cooperative, non-blocking, run-to-completion tasks   */
    /* ================================================================== */
    for (;;)
    {
        /* TEMP bring-up diagnostics: stamp the first 3 WDT1 services and
         * the first refresh, so a capture shows how far each boot gets.   */
        static uint8 diag_svc_count = 0u;
        static bool  diag_refreshed = false;

        /* — Tick / WDT1 ------------------------------------------------ */
        if (scheduler_service() && (diag_svc_count < 3u))
        {
            diag_svc_count++;
            diag_mark("wdt-svc");
        }

        /* — Periodic pipeline (sample → fault → cal → output) ---------- */
        if (scheduler_refresh_pending())
        {
            if (!diag_refreshed)       /* TEMP bring-up diagnostic          */
            {
                diag_refreshed = true;
                diag_mark("refresh");
            }
            acquisition_run(&acq);

            /* System supervision: a dead/stalled ADC or a sagging VDDEXT
             * both make readings untrustworthy while the probes still AGREE
             * (the disagreement check can't catch either). The two causes
             * are tracked independently so the output can report WHICH one
             * is active. On VDDEXT instability, attempt a re-enable —
             * recovers a latched-off regulator (overtemp/undervoltage),
             * not just a sag.                                              */
            if (acq.stalled)
            {
                fault_raise_adc();
            }
            else
            {
                fault_clear_adc();
            }

            if (vddext_stable() || PMU_VDDEXT_On())
            {
                fault_clear_vddext();
            }
            else
            {
                fault_raise_vddext();
            }

            /* Fault check — disagreement only makes sense with two probes */
            if (nvm_config_get_probe_mode() == PROBE_MODE_DUAL)
            {
                fault_check(acq.probe_a, acq.probe_b);
            }
            else
            {
                fault_clear();   /* single probe: clear any latched fault */
            }

            /* Calibration capture accumulator — pause while a fault is
             * active so a corrupted reading can't be averaged into a point */
            if (!fault_is_active())
            {
                cal_state_t before = calibration_get_state();
                calibration_service(acq.combined);
                if (before == CAL_CAPTURING && calibration_get_state() == CAL_ARMED)
                {
                    uart_send_str("Captured (");
                    uart_send_u16(calibration_get_num_points());
                    uart_send_str(" pts)\r\n");
                }
            }

            /* Output stage — fault overrides everything; a valid cal keeps
             * driving the line even while a new cal session is armed (no
             * step back to the raw-counts mapping on the battery line).    */
            if (fault_is_active())
            {
                output_set_fault_low();
            }
            else if (calibration_is_valid())
            {
                output_set_pressure_bar(calibration_apply(acq.combined));
            }
            else
            {
                output_set_pressure(acq.combined);
            }

            /* AUTO stream last, so the line reflects THIS cycle's fault
             * state, output voltage, and drive-path tag.                   */
            uart_cmd_update_readings(acq.probe_a, acq.probe_b, acq.combined);
        }

        /* — Cooperative background tasks -------------------------------- */
        led_arbitrate();
        status_led_service();
        uart_cmd_service();
    }
}
