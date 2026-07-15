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

/* Downhole link + bench console — ONE shared UART:
 * TX P1.0 (alt 3) / RX P1.1 — UART2, NVIC IRQ 11, 9600 8N1. Base setup in
 * link_tx_init (the link OWNS UART2); the debug console adds RX on top.
 * P1.0 is push-pull when driven and HIGH-Z during MCU reset — the harness
 * must provide the idle-high pull (see link_protocol.md).
 * P0.1 (old PWM) and P0.2/UART1 (SWD debug strap) are deliberately unused.  */

/* v2 sync line — GPIO reserved, pin TBD                                     */

/* ---- Operating mode (v2 seam) ------------------------------------------- */
#define MODE_CONTINUOUS         0u
#define MODE_SYNCED_SLEEP       1u      /* v2 — not implemented               */

/* ---- Acquisition -------------------------------------------------------- */
#define OVERSAMPLE_COUNT        16u     /* samples per channel per cycle       */
#define OVERSAMPLE_DIV          4u      /* divide the 16-sample sum by 4, not
                                         * 16: keeps 2 bits of the oversample
                                         * -> 12-bit-SCALED counts, 0-4092.
                                         * ("Effective bits" pending bench
                                         * noise/dither evidence — see
                                         * verification guide ENOB capture.)  */
#define ADC_COUNTS_MAX          4092u   /* = 1023*16/4: MAX OBTAINABLE CODE of
                                         * the oversampling operation. NEVER
                                         * use as a conversion denominator —
                                         * the ideal 12-bit full-scale divisor
                                         * is 4096 (see counts_to_mv).        */
#define ADC_EOC_TIMEOUT_SPINS   400u    /* EOC guard. At -O0 one spin iteration
                                         * is ~2.4 µs (3 non-inlined SDK calls),
                                         * so 400 ≈ 1 ms; healthy conversions
                                         * finish in 1-2 iterations. Keep small:
                                         * a fully-stalled ADC costs
                                         * 34 conv × this per refresh, which
                                         * must stay well under the ~300 ms
                                         * WDT1 service budget               */

/* ---- Downhole link (wire protocol constants live in link_frame.h) ------- */
#ifndef LINK_CONSOLE_EN                 /* overridable from the Keil target    */
#define LINK_CONSOLE_EN         0       /* exp/adc-scope BRANCH: forced 0 to
                                         * free the 1 KB console TX ring for
                                         * the burst buffer (this branch never
                                         * runs the console or the packet
                                         * stream). Mainline default is 1.    */
#endif

/* ---- exp/adc-scope experiment (BENCH ONLY — this branch never deploys) ---
 * Raw single-conversion ADC stream on UART2/P1.0 at 1 Mbaud + RAM burst
 * capture. The logger protocol is NOT on the wire on this branch.           */
#define ADC_SCOPE_BAUD          1000000u /* exact at 40 MHz (80e6/80, 0.00%)  */
#define ADC_SCOPE_BURST_N       1280u    /* uint16 samples = 2.56 KB of the
                                          * 4 KB SRAM — re-check ZI after any
                                          * change (512 B stack must fit too) */
#define ADC_SCOPE_LED_DIV       25000u   /* stream samples per LED toggle:
                                          * ~0.5 s at ~50 kS/s               */
#define LINK_FENCE_TIMEOUT_MS   15u     /* fence: max wait for a safe idle
                                         * window (packet worst case 9.2 ms)  */
#define LINK_CONSOLE_RELOCK_MS  300000u /* 5 min RX inactivity -> auto-relock */
#define LINKTEST_EXPIRY_MS      300000u /* forced test code auto-expiry       */

/* ---- Unit conversion ----------------------------------------------------- *
 * Firmware is bar-native; the bench gauge reads psi. UART accepts a "PSI"
 * suffix on CAL / RANGE values and offers PSI/BAR converter commands.       */
#define BAR_PER_PSI             0.0689476f
#define PSI_PER_BAR             14.5038f

/* ---- Sensor rating -------------------------------------------------------
 * Fixed absolute scale: the wire protocol encodes 0-1000.0 bar in deci-bar
 * (see link_frame.h). The old runtime RANGE window existed only to scale the
 * analog output and was deleted with it. Sensor sensitivity ~0.16 mV/bar is
 * nominal — the amplified front end + multi-point calibration absorb the
 * real transfer function.                                                   */
#define SENSOR_RATING_BAR       1000.0f     /* CAL capture validation ceiling */

/* ---- UART ----------------------------------------------------------------
 * 9600 8N1 — the LOGGER's rate; the shared console runs at it too.          */
#define UART_BAUD               9600u
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
#define PROBE_DISAGREE_DEFAULT  80u     /* ~2 % FS in 12-bit-scaled counts
                                         * (was 20 at 10-bit; UART-settable)  */

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
