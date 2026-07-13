#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*******************************************************************************
 * Pressure Transmitter — compile-time configuration
 *
 * All pin assignments, tuning constants, and TBD placeholders live here.
 * Items marked TODO are waiting on hardware confirmation.
 ******************************************************************************/

/* ---- Pin map (matches PRD §3) ------------------------------------------- */
#define PIN_LED_STATUS          0x04u   /* P0.4 — status LED, push-pull       */

/* ADC1 channels (SDK channel indices, NOT the ANx pin names)                 */
#define ADC_CH_PROBE_A          12u     /* ADC1_CH12 = P2.7 (package AN7)     */
#define ADC_CH_PROBE_B          9u      /* ADC1_CH9  = P2.3 (package AN3)     */

/* PWM output */
/* P0.1 — CCU6 CC62 output (alt 2), configured at runtime by output module   */

/* UART bench port */
/* TX P1.0 (alt 3) / RX P1.1 — UART2, NVIC IRQ 11; pins set up in uart_cmd_init */

/* v2 sync line — GPIO reserved, pin TBD                                     */

/* ---- Operating mode (v2 seam) ------------------------------------------- */
#define MODE_CONTINUOUS         0u
#define MODE_SYNCED_SLEEP       1u      /* v2 — not implemented               */

/* ---- Acquisition -------------------------------------------------------- */
#define OVERSAMPLE_COUNT        16u     /* samples per channel per cycle       */
#define ADC_EOC_TIMEOUT_SPINS   400u    /* EOC guard. At -O0 one spin iteration
                                         * is ~2.4 µs (3 non-inlined SDK calls),
                                         * so 400 ≈ 1 ms; healthy conversions
                                         * finish in 1-2 iterations. Keep small:
                                         * a fully-stalled ADC costs
                                         * 34 conv × this per refresh, which
                                         * must stay well under the ~300 ms
                                         * WDT1 service budget               */

/* ---- Output voltage mapping (TODO: confirm with battery team) ----------- */
#define OUT_V_SUPPLY            5.0f    /* op-amp supply rail, volts           */
#define OUT_V_LO                0.5f    /* pressure-low  voltage              */
#define OUT_V_HI                4.5f    /* pressure-high voltage              */
#define FAULT_V_LO              0.25f   /* fault-low  band (≤ this)           */
#define FAULT_V_HI              4.75f   /* fault-high band (≥ this)           */

/* ---- Unit conversion ----------------------------------------------------- *
 * Firmware is bar-native; the bench gauge reads psi. UART accepts a "PSI"
 * suffix on CAL / RANGE values and offers PSI/BAR converter commands.       */
#define BAR_PER_PSI             0.0689476f
#define PSI_PER_BAR             14.5038f

/* ---- Pressure range (bar, runtime-adjustable) --------------------------- *
 * Units are BAR throughout. The sensor is rated 1000 bar, but the OUTPUT maps
 * the tool's *operating* window [range_lo, range_hi] onto the 0.5-4.5 V
 * sub-range -- so set range_hi to the max the tool will actually see (not the
 * sensor rating) to spend the 10 output bits where it matters. The window is
 * UART-settable (RANGE cmd) + NVM-persisted; these are the power-on defaults.
 * Sensor sensitivity ~0.16 mV/bar is nominal/uncertain -- the multi-point
 * calibration absorbs the real transfer function, so nothing here relies on it.
 * NOTE: 10-bit over the window = span/819 per step (e.g. 0-600 bar -> ~0.73
 * bar/step). ±1-2 bar is realistic at 10-bit; tighter needs 12-bit PWM.        */
#define RANGE_LO_BAR_DEFAULT    1.0f        /* maps to OUT_V_LO (0.5 V); ambient ~1.013 bar */
#define RANGE_HI_BAR_DEFAULT    1000.0f     /* maps to OUT_V_HI (4.5 V); lower to tool max  */
#define RANGE_BAR_FLOOR         0.0f        /* validation: lo >= this               */
#define RANGE_BAR_CEIL          1000.0f     /* validation: hi <= sensor rating       */
#define RANGE_MIN_SPAN_BAR      1.0f        /* validation: hi - lo >= this           */

/* ---- PWM DAC (TODO: finalize after hardware filter) --------------------- *
 * NOTE: the actual frequency is set by output_init(): fCCU6/2 = 20 MHz over
 * a 1024-step period = 19.531 kHz. PWM_FREQ_HZ is INFORMATIONAL ONLY --
 * changing it does not reprogram the timer (revisit when the hardware
 * filter values land and the divider is worth deriving).                    */
#define PWM_FREQ_HZ             19531u  /* actual: 20 MHz / 1024 (see note)   */
#define PWM_RESOLUTION_BITS     10u     /* match ADC effective bits           */
#define PWM_MAX_COUNT           (1u << PWM_RESOLUTION_BITS)

/* ---- UART --------------------------------------------------------------- */
#define UART_BAUD               115200u
#define UART_TX_BUF_SIZE        1024u   /* ISR TX ring buffer — must hold the
                                         * largest single burst (HELP ~800 B);
                                         * uart_putc DROPS bytes when full     */
#define UART_RX_BUF_SIZE        128u    /* ISR RX ring buffer                 */
#define UART_CMD_BUF_SIZE       80u     /* max command line length            */

/* ---- Calibration -------------------------------------------------------- */
#define CAL_NVM_ADDR            0x1100FF80u /* last 128-byte page of flash    */
#define CAL_MAX_POINTS          8u          /* max capture points             */
#define CAL_CAPTURE_SAMPLES     8u          /* refresh cycles to avg per pt   */

/* ---- NVM settings ------------------------------------------------------- */
#define SETTINGS_NVM_ADDR       0x1100FF00u /* second-to-last page            */

/* ---- Fault thresholds --------------------------------------------------- */
#define PROBE_DISAGREE_DEFAULT  20u     /* ~2 % FS at 10-bit (UART-settable)  */

/* ---- Probe source (UART-settable, NVM-persisted) ------------------------ */
#define PROBE_MODE_DUAL         0u      /* average ProbeA & ProbeB (default)  */
#define PROBE_MODE_A            1u      /* single probe on AN7 (P2.7) only    */
#define PROBE_MODE_B            2u      /* single probe on AN3 (P2.3) only    */
#define PROBE_MODE_DEFAULT      PROBE_MODE_DUAL

/* ---- Power table (v1: single constant, characterize on hardware) -------- */
#define POWER_CONTINUOUS_MW     40u     /* placeholder — measure on HW        */

/* ---- v2 sync line (reserved, pin TBD) ----------------------------------- */
/* #define PIN_SYNC_GPIO        TBDu */

/* ---- Refresh / sample rate ---------------------------------------------- */
#define REFRESH_RATE_DEFAULT_MS 1000u   /* 1 Hz — arbitrary for now           */
#define REFRESH_RATE_MIN_MS     100u    /* fastest  (10 Hz)                   */
#define REFRESH_RATE_MAX_MS     5000u   /* slowest  (0.2 Hz)                  */

/* ---- Scheduler / tick --------------------------------------------------- */
#define TICK_MS                 1u      /* SysTick period (from SDK wdt1.h)   */

/* ---- VDDEXT settle ------------------------------------------------------ */
#define VDDEXT_SETTLE_TIMEOUT_MS 100u   /* bound the boot wait — never hang   */

/* ---- LED timing (ms) ---------------------------------------------------- */
#define LED_HEARTBEAT_ON_MS     100u
#define LED_HEARTBEAT_OFF_MS    2900u
#define LED_CAL_ARMED_MS        500u    /* 1 Hz toggle                        */
#define LED_CAL_CAPTURE_MS      100u    /* 5 Hz toggle                        */
#define LED_CAL_STORED_MS       2000u   /* solid-on duration                  */
#define LED_FAULT_ON_MS         100u    /* double-blink: on                   */
#define LED_FAULT_GAP_MS        100u    /* double-blink: inter-blink gap      */
#define LED_FAULT_PAUSE_MS      600u    /* double-blink: inter-pair pause     */

#endif /* APP_CONFIG_H */
