#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/*******************************************************************************
 * Pressure Transmitter — compile-time configuration
 *
 * =========================== BOARD HARDWARE REV 2 ===========================
 * THIS FIRMWARE IS FOR BOARD HARDWARE REV 2 AND ONLY REV 2.
 * Every other firmware branch in this repo targets board Rev 1.
 *
 * Do not confuse this with TLE9854_pressure_transmitter_PRD_rev2.md — that
 * is the REQUIREMENTS-DOCUMENT revision and it applies to both boards. The
 * two numbering schemes are unrelated. Always write "board Rev 2" or "PRD
 * Rev 2"; bare "Rev 2" in this repo is ambiguous — it already means the PRD.
 *
 * What board Rev 2 changed:
 *   - Adds the onboard IS25LP128F NOR flash on SSC1 (P0.3/P0.4/P0.5/P1.2).
 *     Rev 1 has no flash — the whole spi_nor module is Rev-2-only.
 *   - Status LED moved P0.4 -> P1.4, forced by the above: P0.4 became
 *     SSC1_M_MTSR (flash SI).
 *
 * Cross-flashing fails SILENTLY — there is no revision strap to read and
 * nothing detects the mismatch at runtime:
 *   - This image on a Rev 1 board: the LED is driven on P1.4, which Rev 1
 *     does not wire to the LED, so the LED stays dark and every LED-based
 *     diagnostic in verification_guide.md quietly lies. SSC1 meanwhile
 *     drives P0.4 — which on Rev 1 IS the LED.
 *   - A Rev 1 image on a Rev 2 board: LED output drives the flash SI line.
 *
 * CONFIRM THE BOARD REVISION BEFORE YOU FLASH.
 * ===========================================================================
 *
 * All pin assignments, tuning constants, and TBD placeholders live here.
 * Items marked TODO are waiting on hardware confirmation.
 ******************************************************************************/

/* ---- Pin map (BOARD REV 2 — matches PRD §3) -------------------------------------------- *
 * PIN_* values use the PORT_ChangePinAlt() encoding: high nibble = port,
 * low nibble = pin (so P1.4 == 0x14).
 *
 *   P0.3 (27)  SSC1_M_SCK   ALT1   NOR flash SCK
 *   P0.4 (28)  SSC1_M_MTSR  ALT1   NOR flash SI   (status LED on Rev 1)
 *   P0.5 (29)  SSC1_M_MRST  INP1   NOR flash SO
 *   P1.2 (33)  GPIO out            NOR flash CE#
 *   P1.4 (34)  GPIO out            status LED
 *   P1.0 (31)  UART2_TXD    ALT3   downhole link TX
 *   P1.1 (32)  UART2_RXD    INP6   bench console RX
 *   P2.7 / P2.3                    Probe A / Probe B (ADC1)
 *                                                                            */
#define PIN_LED_STATUS          0x14u   /* P1.4 — status LED, push-pull.
                                         * BOARD REV 2 PIN. Rev 1 has the LED
                                         * on P0.4; it moved because Rev 2
                                         * made P0.4 SSC1_M_MTSR (flash SI).
                                         * Wrong on a Rev 1 board.
                                         * DOCUMENTATION ONLY — the SDK has
                                         * one inline fn per pin, so
                                         * status_led.c names PORT_P14_*
                                         * literally. Change both together.   */

/* ADC1 channels (SDK channel indices, NOT the ANx pin names)                 */
#define ADC_CH_PROBE_A          12u     /* ADC1_CH12 = P2.7 (package AN7)     */
#define ADC_CH_PROBE_B          9u      /* ADC1_CH9  = P2.3 (package AN3)     */

/* Downhole link + bench console — ONE shared UART:
 * TX P1.0 (alt 3) / RX P1.1 — UART2, NVIC IRQ 11, 9600 8N1. Base setup in
 * link_tx_init (the link OWNS UART2); the debug console adds RX on top.
 * P1.0 is push-pull when driven and HIGH-Z during MCU reset — the harness
 * must provide the idle-high pull (see link_protocol.md).
 * P0.1 (old PWM) and P0.2/UART1 (SWD debug strap) are deliberately unused.
 * SSC2 is UNUSABLE for the flash: its master pins are SSC2_M_SCK on P1.0
 * ALT1 and SSC2_M_MTSR on P1.1 ALT1 — a direct collision with this UART.
 * The NOR flash therefore uses SSC1 (see PIN_NOR_* below).                   */

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
#define LINK_CONSOLE_EN         1       /* 1 = debug build: bench console
                                         * compiled in (boots LOCKED, unlock
                                         * suspends the stream). 0 = PRODUCTION:
                                         * console compiled out entirely —
                                         * packets are the only possible
                                         * bytes on the wire. Set via a -D
                                         * define in the production target.   */
#endif
#define LINK_FENCE_TIMEOUT_MS   15u     /* fence: max wait for a safe idle
                                         * window (packet worst case 9.2 ms)  */
#define LINK_CONSOLE_RELOCK_MS  300000u /* 5 min RX inactivity -> auto-relock */
#define LINKTEST_EXPIRY_MS      300000u /* forced test code auto-expiry       */

/* ---- Onboard NOR flash (IS25LP128F, 128 Mbit / 16 MB) on SSC1 ------------ *
 * Signal names follow the IS25LP128F datasheet: CE# (not CS#), SI (not
 * MOSI), SO (not MISO).
 *
 * SSC1, not SSC2 — SSC2's master pins sit on P1.0/P1.1, which belong to the
 * downhole link. The RTE has no ssc.c/ssc.h (only ssc_defines.h), so
 * spi_nor.c drives the SSC1_Type register block at 0x48024000 directly;
 * this needs ZERO RTE edits, same as the link rearchitecture.
 *
 * Board note: WP# and HOLD#/RESET# must be held high for standard SPI mode.
 * Floating pins produce intermittent page-program failures that look exactly
 * like a driver bug.                                                        */
#define PIN_NOR_SCK             0x03u   /* P0.3 (27) ALT1 SSC1_M_SCK          */
#define PIN_NOR_SI              0x04u   /* P0.4 (28) ALT1 SSC1_M_MTSR         */
#define PIN_NOR_SO              0x05u   /* P0.5 (29) INP1 SSC1_M_MRST         */
#define PIN_NOR_CE              0x12u   /* P1.2 (33) plain GPIO, active low   */
#define NOR_ALT_SSC1            1u      /* ALT1 selects the SSC1 mapping on
                                         * P0.3 / P0.4 (see datasheet Tbl 6)  */

/* SSC1 shift clock. The datasheet's SSC chapter (20) is overview-only — no
 * register semantics and no baud formula — but the Config Wizard defaults in
 * RTE/.../ssc_defines.h pin it down: SSC1_BR = 19 paired with
 * SSC1_MAN_BAUDRATE = 1000 (kBaud) implies
 *
 *      f_SSC = 40 MHz / (2 * (BR_VALUE + 1))
 *
 * which also reproduces the datasheet's quoted 250 kBaud - 8 MBaud range
 * (BR=79 -> 250 kHz, BR=4 -> 4 MHz) and matches the 40 MHz system clock.
 * BR=19 -> 1 MHz: deliberately slow for bring-up. Do not raise it until the
 * JEDEC ID reads clean (IS25LP128F itself is good to 133 MHz).              */
#define NOR_SSC1_BR             19u     /* -> 1 MHz shift clock               */

/* Bounded guard for the probe-sized blocking transfers in spi_nor.c. One
 * byte at 1 MHz is ~8 us; at -O0 a spin iteration is ~1 us, so 400 is ~50x
 * the expected wait. Never spin unbounded on a status bit (hard constraint). */
#define NOR_SPIN_GUARD          400u

/* SPI mode 0 (CPOL=0, CPHA=0) is what the IS25LP128F wants. PO=0 is certain
 * (idle-low clock); the PH bit's polarity is a WORKING HYPOTHESIS — the
 * local datasheet never defines it, same class of unknown as the TI timing
 * hypothesis in link_frame.h. spi_nor_probe() therefore tries this value
 * first and falls back to the opposite, reporting which one answered. Once
 * the bench settles it, set this and drop the fallback.                     */
#define NOR_SSC1_PH             1u      /* hypothesis: latch on leading edge  */

/* Internal pull-up on SO (P0.5). WHICH TRANSLATOR SO RUNS THROUGH DECIDES
 * THIS — it is not a free choice:
 *
 *   1 = SO on a UNIDIRECTIONAL channel (TXU0304). The pull is harmless and
 *       useful: a silent flash then reads a clean FF FF FF instead of a
 *       floating input drifting low and impersonating a driver fault.
 *
 *   0 = SO on the AUTO-DIRECTION translator (NTB0104). These sense direction
 *       by driving weakly and watching for something external to overpower
 *       them, so an internal pull of a few tens of kΩ can wedge the sensing
 *       in the wrong direction. The pull MUST be off.
 *
 * Cost of 0: an absent flash reads an ambiguous level rather than all-ones,
 * so lean on LOG LOOP and LOG PINS for that diagnosis instead.             */
#define NOR_SO_PULLUP           1u

/* Bench-op timeouts. Generous vs the IS25LP128F's own numbers (page program
 * a few ms, 4 KB sector erase tens to low hundreds) — these exist to bound a
 * dead or wedged part, not to police normal timing. The wait loop feeds
 * WDT1, so a sector erase outrunning the ~300 ms budget is safe.           */
#define NOR_WAIT_PP_MS          50u     /* page program                       */
#define NOR_WAIT_SE_MS          1000u   /* 4 KB sector erase                  */

/* Where LOG TEST does its erase/program/read-back round trip. Sector 0 —
 * the datalog will erase the whole part later anyway.                       */
/* LOG PINS: how long each signal is held at a DC level. Long enough to
 * settle a multimeter, short enough that the whole sweep is ~12 s.     */
#define NOR_DIAG_HOLD_MS        2000u

#define NOR_TEST_ADDR           0x000000u
#define NOR_TEST_LEN            16u

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
