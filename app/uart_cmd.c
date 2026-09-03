#include "uart_cmd.h"
#include "app_config.h"
#include "scheduler.h"
#include "output.h"
#include "calibration.h"
#include "nvm_config.h"
#include "fault.h"
#include "acquisition.h"
#include "uart.h"
#include "port.h"
#include "wdt1.h"

/* ---- Ring buffers ------------------------------------------------------- */
static volatile uint8  tx_buf[UART_TX_BUF_SIZE];
static volatile uint16 tx_head, tx_tail;
static volatile bool   tx_busy;

static volatile uint8  rx_buf[UART_RX_BUF_SIZE];
static volatile uint16 rx_head, rx_tail;

/* ---- Command line buffer ------------------------------------------------ */
static uint8  cmd_buf[UART_CMD_BUF_SIZE];
static uint8  cmd_len;

/* ---- Latest readings (updated by main loop) ----------------------------- */
static uint16 rd_probe_a;
static uint16 rd_probe_b;
static uint16 rd_combined;

/* ---- State -------------------------------------------------------------- */
static bool   auto_print;

/* ---- Forward declarations ----------------------------------------------- */
static void process_cmd(const char *cmd);
static void uart_putc(uint8 c);
static bool cmd_eq(const char *a, const char *b);
static bool cmd_prefix(const char *cmd, const char *prefix);
static uint16 parse_u16(const char *s);
static float  parse_float(const char *s);
static void   send_volts(uint16 duty);
static const char *output_state(void);

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */
void uart_cmd_init(void)
{
    tx_head = 0u;  tx_tail = 0u;  tx_busy = false;
    rx_head = 0u;  rx_tail = 0u;
    cmd_len = 0u;
    auto_print = false;

    /* P1.0 = UART2 TXD (alt 3, output), P1.1 = UART2 RXD (input, fixed) */
    PORT_P10_Output_Set();
    PORT_ChangePinAlt(0x10u, 3u);      /* P1.0 → UART2 TX (alt 3) */

    /* Config Wizard leaves SCON = 0 (synchronous mode).
     * Set SM1 = 1 for Mode 1: 8-bit UART, variable baud rate. */
    UART2->SCON.reg |= (uint32)(1u << 6u);

    UART2_BaudRate_Set(UART_BAUD);
    UART2_Receiver_En();

    /* Drain any spurious byte latched while the Rx line settled, and clear the
     * RX flag, so the FIRST real command isn't corrupted by leading garbage. */
    (void)UART2_Buffer_Get();
    UART2_RX_Int_Clr();

    /* Enable UART2 NVIC (IRQ 11) and module-level RX interrupt.
     * TX interrupt is enabled on-demand when data is queued. */
    CPU->NVIC_ISER.reg |= (1u << 11u);
    UART2_RX_Int_En();

    uart_send_str("\r\n== Pressure Transmitter v1 ==\r\n");
}

void uart_cmd_service(void)
{
    /* --- Pull bytes from RX ring buffer into command line buffer --------- */
    while (rx_head != rx_tail)
    {
        uint8 b = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1u) % UART_RX_BUF_SIZE;

        if (b == '\r' || b == '\n')
        {
            /* Process on Enter. Blank lines (and the LF of a CR-LF pair, since
             * cmd_len is already 0) are ignored without echoing. */
            if (cmd_len > 0u)
            {
                uart_send_str("\r\n");
                /* Trim trailing spaces so "STATUS " matches STATUS. */
                while (cmd_len > 0u && cmd_buf[cmd_len - 1u] == (uint8)' ')
                {
                    cmd_len--;
                }
                cmd_buf[cmd_len] = '\0';
                process_cmd((const char *)cmd_buf);
                cmd_len = 0u;
                /* At most ONE command per service pass: a pasted batch of
                 * RAW/SCAN lines would otherwise run hundreds of conversions
                 * between WDT1 services. Remaining bytes wait one loop pass. */
                break;
            }
        }
        else if (b == 0x08u || b == 0x7Fu)      /* backspace / delete */
        {
            if (cmd_len > 0u)
            {
                cmd_len--;
                uart_send_str("\b \b");         /* erase the char on the terminal */
            }
        }
        else if (b >= 0x20u && b <= 0x7Eu)      /* printable ASCII only */
        {
            if (cmd_len < (UART_CMD_BUF_SIZE - 1u))
            {
                cmd_buf[cmd_len++] = b;
                uart_putc(b);                   /* echo so the user sees input */
            }
            /* else: line full -> drop extra chars silently */
        }
        /* else: control / line-noise byte -> ignore (kills startup garbage) */
    }
}

void uart_cmd_update_readings(uint16 probe_a, uint16 probe_b, uint16 combined)
{
    rd_probe_a  = probe_a;
    rd_probe_b  = probe_b;
    rd_combined = combined;

    if (auto_print)
    {
        /* Debug stream: counts + mV per probe, pressure (if cal'd), the actual
         * output voltage, and which path is driving it (FLT/MAN/CAL/RAW). */
        uart_send_str("A:");     uart_send_u16(probe_a);
        uart_send_str(" ");      uart_send_u16(acquisition_counts_to_mv(probe_a));
        uart_send_str("mV  B:"); uart_send_u16(probe_b);
        uart_send_str(" ");      uart_send_u16(acquisition_counts_to_mv(probe_b));
        uart_send_str("mV  Avg:"); uart_send_u16(combined);

        uart_send_str("  P:");
        if (calibration_is_valid())
        {
            uart_send_float3(calibration_apply(combined));
            uart_send_str("bar");
        }
        else
        {
            uart_send_str("uncal");
        }

        uart_send_str("  Out:");
        send_volts(output_get_duty());
        uart_send_str("V ");
        uart_send_str(output_state());
        uart_send_str("\r\n");
    }
}

void uart_send_str(const char *s)
{
    while (*s != '\0')
    {
        uart_putc((uint8)*s);
        s++;
    }
}

/* ========================================================================= */
/*  ISR callbacks (called from UART2_IRQHandler in isr.c)                    */
/* ========================================================================= */
void uart_cmd_rx_isr(void)
{
    uint8  b    = UART2_Buffer_Get();
    uint16 next = (rx_head + 1u) % UART_RX_BUF_SIZE;
    if (next != rx_tail)            /* room in buffer */
    {
        rx_buf[rx_head] = b;
        rx_head = next;
    }
    /* overflow: drop byte */
}

void uart_cmd_tx_isr(void)
{
    if (tx_head != tx_tail)
    {
        UART2_Buffer_Set(tx_buf[tx_tail]);
        tx_tail = (tx_tail + 1u) % UART_TX_BUF_SIZE;
    }
    else
    {
        tx_busy = false;
        UART2_TX_Int_Dis();
    }
}

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */
static void uart_putc(uint8 c)
{
    uint16 next = (tx_head + 1u) % UART_TX_BUF_SIZE;
    if (next == tx_tail) { return; }  /* buffer full — drop */

    __disable_irq();
    tx_buf[tx_head] = c;
    tx_head = next;

    if (!tx_busy)
    {
        tx_busy = true;
        /* Prime the transmitter with the first byte. */
        UART2_Buffer_Set(tx_buf[tx_tail]);
        tx_tail = (tx_tail + 1u) % UART_TX_BUF_SIZE;
        UART2_TX_Int_En();
    }
    __enable_irq();
}

void uart_send_u16(uint16 val)
{
    char  buf[6];
    uint8 i = 0u;
    if (val == 0u) { uart_putc('0'); return; }
    while (val > 0u)
    {
        buf[i++] = (char)('0' + (val % 10u));
        val /= 10u;
    }
    while (i > 0u) { uart_putc((uint8)buf[--i]); }
}

void uart_send_i32(sint32 val)
{
    if (val < 0) { uart_putc((uint8)'-'); val = -val; }
    uart_send_u16((uint16)val);
}

void uart_tx_flush_bounded(void)
{
    uint32 t0 = scheduler_get_ms();
    while (tx_busy || (tx_head != tx_tail))
    {
        (void)WDT1_Service();
        if ((scheduler_get_ms() - t0) >= 200u) { break; }   /* 1 KB drains in <90 ms */
    }
}

void uart_send_hex16(uint16 val)
{
    static const char hex[] = "0123456789ABCDEF";
    uart_putc((uint8)hex[(val >> 12) & 0xFu]);
    uart_putc((uint8)hex[(val >>  8) & 0xFu]);
    uart_putc((uint8)hex[(val >>  4) & 0xFu]);
    uart_putc((uint8)hex[val & 0xFu]);
}

void uart_send_float3(float f)
{
    uint16 integer;
    uint16 frac;
    if (f < 0.0f) { uart_putc('-'); f = -f; }
    integer = (uint16)f;
    frac = (uint16)((f - (float)integer) * 1000.0f + 0.5f);
    if (frac >= 1000u) { integer++; frac = 0u; }
    uart_send_u16(integer);
    uart_putc('.');
    if (frac < 100u) { uart_putc('0'); }
    if (frac < 10u)  { uart_putc('0'); }
    uart_send_u16(frac);
}

static bool cmd_eq(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') { ca -= 32; }
        if (cb >= 'a' && cb <= 'z') { cb -= 32; }
        if (ca != cb) { return false; }
        a++; b++;
    }
    return (*a == '\0' && *b == '\0');
}

/* Case-insensitive prefix match (so "Rate 500" works, not just RATE/rate). */
static bool cmd_prefix(const char *cmd, const char *prefix)
{
    while (*prefix)
    {
        char cc = *cmd, cp = *prefix;
        if (cc >= 'a' && cc <= 'z') { cc -= 32; }
        if (cp >= 'a' && cp <= 'z') { cp -= 32; }
        if (cc != cp) { return false; }
        cmd++; prefix++;
    }
    return true;
}

static uint16 parse_u16(const char *s)
{
    uint32 v = 0u;          /* 32-bit accumulator: saturate instead of wrapping
                             * so e.g. "65636" can't alias into a valid range */
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10u + (uint32)(*s - '0');
        if (v > 0xFFFFu) { return 0xFFFFu; }
        s++;
    }
    return (uint16)v;
}

/* Strictly digits, at least one — rejects "" , "abc", and "10O" (which
 * parse_u16 would silently read as 10). */
static bool is_clean_u16(const char *s)
{
    if (*s < '0' || *s > '9') { return false; }
    while (*s != '\0')
    {
        if (*s < '0' || *s > '9') { return false; }
        s++;
    }
    return true;
}

/* Parse "<float>[ PSI]" into bar. The gauge on the bench reads psi; the
 * firmware is bar-native, so a PSI suffix converts at the parser and
 * everything downstream stays bar. Returns false on malformed input
 * (non-numeric start, trailing garbage that isn't a PSI suffix). */
static bool parse_pressure_bar(const char *s, float *out_bar, bool *was_psi)
{
    float v;

    if (!((*s >= '0' && *s <= '9') || *s == '.')) { return false; }
    v = parse_float(s);
    while (*s == '.' || (*s >= '0' && *s <= '9')) { s++; }
    while (*s == ' ') { s++; }

    if (*s == '\0')
    {
        *was_psi = false;
        *out_bar = v;
        return true;
    }
    if (cmd_eq(s, "PSI"))
    {
        *was_psi = true;
        *out_bar = v * BAR_PER_PSI;
        return true;
    }
    return false;
}

static float parse_float(const char *s)
{
    float result = 0.0f;
    float div    = 1.0f;
    bool  dot    = false;
    bool  neg    = false;

    if (*s == '-') { neg = true; s++; }
    while (*s != '\0')
    {
        if (*s == '.' && !dot) { dot = true; s++; continue; }
        if (*s < '0' || *s > '9') { break; }
        if (dot) { div *= 10.0f; result += (float)(*s - '0') / div; }
        else     { result = result * 10.0f + (float)(*s - '0'); }
        s++;
    }
    return neg ? -result : result;
}

/* Current PWM duty -> filtered output voltage (assumes 0..OUT_V_SUPPLY span). */
static void send_volts(uint16 duty)
{
    uart_send_float3((float)duty / (float)(PWM_MAX_COUNT - 1u) * OUT_V_SUPPLY);
}

/* Which path is actually driving the output, in priority order. */
static const char *output_state(void)
{
    if (fault_is_active())      { return "FLT"; }
    if (output_is_manual())     { return "MAN"; }
    if (calibration_is_valid()) { return "CAL"; }
    return "RAW";
}

static void print_status(void)
{
    uint16 pm = nvm_config_get_probe_mode();

    uart_send_str("ProbeA: ");   uart_send_u16(rd_probe_a);
    uart_send_str("  ProbeB: "); uart_send_u16(rd_probe_b);
    uart_send_str("  Avg: ");    uart_send_u16(rd_combined);
    uart_send_str("  Probe: ");
    uart_send_str((pm == PROBE_MODE_A) ? "A" : ((pm == PROBE_MODE_B) ? "B" : "AVG"));
    uart_send_str("\r\n");

    uart_send_str("Output: ");   send_volts(output_get_duty());
    uart_send_str("V  ");        uart_send_str(output_is_manual() ? "MANUAL" : "AUTO");
    uart_send_str("  Fault: ");  uart_send_str(fault_is_active() ? "YES" : "no");
    uart_send_str("\r\n");

    uart_send_str("Rate: ");       uart_send_u16((uint16)scheduler_get_rate_ms());
    uart_send_str("ms  Thresh: "); uart_send_u16(nvm_config_get_disagree_thresh());
    uart_send_str("  Range: ");    uart_send_float3(nvm_config_get_range_lo_bar());
    uart_send_str("-");            uart_send_float3(nvm_config_get_range_hi_bar());
    uart_send_str(" bar\r\n");

    uart_send_str("Cal: ");
    if (calibration_is_valid())
    {
        uart_send_str("VALID  slope=");
        uart_send_float3(calibration_get_slope());
        uart_send_str(" offset=");
        uart_send_float3(calibration_get_offset());
    }
    else
    {
        uart_send_str("NONE");
    }
    uart_send_str("\r\n");
}

static void print_chan_raw(const char *name, const acq_chan_debug_t *c)
{
    uart_send_str(name);
    uart_send_str(" avg=");  uart_send_u16(c->avg);
    uart_send_str(" min=");  uart_send_u16(c->min);
    uart_send_str(" max=");  uart_send_u16(c->max);
    uart_send_str(" mV=");   uart_send_u16(c->mv);
    uart_send_str(" valid="); uart_send_u16(c->valid);
    uart_send_str("/");      uart_send_u16(OVERSAMPLE_COUNT);
    uart_send_str("\r\n");
}

static void print_raw(void)
{
    acq_debug_t d;
    acquisition_debug(&d);
    uart_send_str("RAW (P2.x inputs, 1 LSB ~5mV):\r\n");
    print_chan_raw("  A:", &d.a);
    print_chan_raw("  B:", &d.b);
}

static void print_scan(void)
{
    /* Every external analog input on this package (ADC1 ch -> AN -> P2.x).      */
    static const uint8 chans[5] = { 6u, 7u, 8u, 9u, 12u };
    static const char  *names[5] = {
        "AN0(P2.0)   ", "AN1(P2.1)   ", "AN2(P2.2)   ",
        "AN3(P2.3) <-B", "AN7(P2.7) <-A"
    };
    acq_chan_debug_t c;
    uint8 i;

    uart_send_str("SCAN all analog inputs (<- = used by firmware):\r\n");
    for (i = 0u; i < 5u; i++)
    {
        acquisition_scan_channel(chans[i], &c);
        uart_send_str("  ");
        uart_send_str(names[i]);
        uart_send_str(" cnt="); uart_send_u16(c.avg);
        uart_send_str(" mV=");  uart_send_u16(c.mv);
        uart_send_str("\r\n");
    }
}

static void process_cmd(const char *cmd)
{
    if (cmd_eq(cmd, "STATUS"))
    {
        print_status();
    }
    else if (cmd_eq(cmd, "RAW"))
    {
        print_raw();
    }
    else if (cmd_eq(cmd, "SCAN"))
    {
        print_scan();
    }
    else if (cmd_eq(cmd, "AUTO"))
    {
        auto_print = !auto_print;
        uart_send_str(auto_print ? "Auto ON\r\n" : "Auto OFF\r\n");
    }
    else if (cmd_prefix(cmd, "THRESH "))
    {
        uint16 t = parse_u16(cmd + 7);
        if (is_clean_u16(cmd + 7) && t > 0u && t <= 1023u)
        {
            nvm_config_set_disagree_thresh(t);
            if (nvm_config_save())
            {
                uart_send_str("Thresh=");
                uart_send_u16(t);
                uart_send_str(" (saved)\r\n");
            }
            else
            {
                uart_send_str("Thresh=");
                uart_send_u16(t);
                uart_send_str(" (NVM write failed)\r\n");
            }
        }
        else
        {
            uart_send_str("ERR: thresh 1-1023\r\n");
        }
    }
    else if (cmd_prefix(cmd, "RATE "))
    {
        uint16 ms = parse_u16(cmd + 5);
        if (is_clean_u16(cmd + 5) && ms >= REFRESH_RATE_MIN_MS && ms <= REFRESH_RATE_MAX_MS)
        {
            /* Validate BEFORE persisting: the live and saved values must be
             * the same number, or the setting silently changes on reboot.  */
            scheduler_set_rate_ms((uint32)ms);
            nvm_config_set_rate_ms((uint32)ms);
            if (nvm_config_save())
            {
                uart_send_str("Rate=");
                uart_send_u16(ms);
                uart_send_str("ms (saved)\r\n");
            }
            else
            {
                uart_send_str("Rate=");
                uart_send_u16(ms);
                uart_send_str("ms (NVM write failed)\r\n");
            }
        }
        else
        {
            uart_send_str("ERR: rate 100-10000\r\n");
        }
    }
    else if (cmd_prefix(cmd, "RANGE "))
    {
        const char *p = cmd + 6;
        float lo, hi;
        bool  ok;

        /* Both operands must actually start numeric — parse_float reads
         * garbage as 0.0, which would silently persist a wrong window.     */
        while (*p == ' ') { p++; }
        ok = ((*p >= '0' && *p <= '9') || *p == '.');
        lo = parse_float(p);
        /* step past the first number, then any spaces, to the second value */
        while (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')) { p++; }
        while (*p == ' ') { p++; }
        ok = ok && ((*p >= '0' && *p <= '9') || *p == '.');
        hi = parse_float(p);

        /* Optional unit suffix: "RANGE <lo> <hi> PSI" converts both. Any
         * other trailing text is rejected.                                 */
        while (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')) { p++; }
        while (*p == ' ') { p++; }
        if (*p != '\0')
        {
            if (cmd_eq(p, "PSI")) { lo *= BAR_PER_PSI; hi *= BAR_PER_PSI; }
            else                  { ok = false; }
        }

        if (ok && lo >= RANGE_BAR_FLOOR && hi <= RANGE_BAR_CEIL &&
            (hi - lo) >= RANGE_MIN_SPAN_BAR)
        {
            nvm_config_set_range_bar(lo, hi);
            if (nvm_config_save())
            {
                uart_send_str("Range=");
                uart_send_float3(lo);
                uart_send_str("-");
                uart_send_float3(hi);
                uart_send_str(" bar (saved)\r\n");
            }
            else
            {
                uart_send_str("Range set (NVM write failed)\r\n");
            }
        }
        else
        {
            uart_send_str("ERR: RANGE <lo> <hi> [PSI]  (0<=lo<hi<=1000 bar)\r\n");
        }
    }
    else if (cmd_prefix(cmd, "PROBE "))
    {
        const char *arg = cmd + 6;
        uint16 m  = PROBE_MODE_DUAL;
        bool   ok = true;
        if      (cmd_eq(arg, "A"))                          { m = PROBE_MODE_A; }
        else if (cmd_eq(arg, "B"))                          { m = PROBE_MODE_B; }
        else if (cmd_eq(arg, "AVG") || cmd_eq(arg, "DUAL")) { m = PROBE_MODE_DUAL; }
        else                                                { ok = false; }

        if (ok)
        {
            nvm_config_set_probe_mode(m);
            if (nvm_config_save())
            {
                uart_send_str("Probe=");
                uart_send_str((m == PROBE_MODE_A) ? "A" : ((m == PROBE_MODE_B) ? "B" : "AVG"));
                uart_send_str(" (saved)\r\n");
            }
            else
            {
                uart_send_str("Probe set (NVM write failed)\r\n");
            }
        }
        else
        {
            uart_send_str("ERR: PROBE A|B|AVG\r\n");
        }
    }
    else if (cmd_prefix(cmd, "OUTPUT "))
    {
        const char *arg = cmd + 7;
        if (cmd_eq(arg, "AUTO"))
        {
            output_set_auto();
            uart_send_str("Output AUTO\r\n");
        }
        else if (is_clean_u16(arg))            /* reject non/partial-numeric:
                                                * "abc" parses as 0, "10O" as
                                                * 10 — both would silently
                                                * latch a wrong override     */
        {
            uint16 c = parse_u16(arg);
            if (c <= 1023u)
            {
                output_set_manual(c);
                if (fault_is_active())
                {
                    /* Fault has priority on the line — the manual value is
                     * latched and takes effect once the fault clears.      */
                    output_set_fault_low();
                    uart_send_str("Output=");
                    uart_send_u16(c);
                    uart_send_str(" (latched; fault active, line stays fault-low)\r\n");
                }
                else
                {
                    uart_send_str("Output=");
                    uart_send_u16(c);
                    uart_send_str("\r\n");
                }
            }
            else
            {
                uart_send_str("ERR: output 0-1023\r\n");
            }
        }
        else
        {
            uart_send_str("ERR: OUTPUT <0-1023>|AUTO\r\n");
        }
    }
    else if (cmd_prefix(cmd, "CAL "))
    {
        const char *arg = cmd + 4;
        bool capturing = (calibration_get_state() == CAL_CAPTURING);

        if (cmd_eq(arg, "ARM"))
        {
            if (capturing)
            {
                uart_send_str("ERR: capture in progress, wait for 'Captured'\r\n");
            }
            else
            {
                calibration_arm();
                uart_send_str("Cal ARMED (");
                uart_send_u16(calibration_get_num_points());
                uart_send_str(" pts)\r\n");
            }
        }
        else if (cmd_eq(arg, "STORE"))
        {
            if (capturing)
            {
                /* Storing now would silently drop the in-flight point. */
                uart_send_str("ERR: capture in progress, wait for 'Captured'\r\n");
            }
            else
            {
                switch (calibration_store())
                {
                case CAL_STORE_OK:
                    uart_send_str("Cal stored: slope=");
                    uart_send_float3(calibration_get_slope());
                    uart_send_str(" offset=");
                    uart_send_float3(calibration_get_offset());
                    uart_send_str("\r\n");
                    break;
                case CAL_STORE_TOO_FEW:
                    uart_send_str("ERR: need >=2 pts\r\n");
                    break;
                case CAL_STORE_BAD_FIT:
                    uart_send_str("ERR: degenerate fit (points at same counts)\r\n");
                    break;
                default:   /* CAL_STORE_NVM_FAIL */
                    uart_send_str("ERR: NVM write failed\r\n");
                    break;
                }
            }
        }
        else if (cmd_eq(arg, "CLEAR"))
        {
            if (calibration_clear())
            {
                uart_send_str("Cal cleared\r\n");
            }
            else
            {
                uart_send_str("Cal cleared (RAM only - NVM erase failed, may return after reset)\r\n");
            }
        }
        else if (cmd_eq(arg, "ABORT"))
        {
            calibration_abort();
            uart_send_str("Cal aborted\r\n");
        }
        else if (cmd_eq(arg, "STATUS"))
        {
            uart_send_str("Cal: ");
            uart_send_str(calibration_is_valid() ? "VALID" : "NONE");
            uart_send_str("  pts=");
            uart_send_u16(calibration_get_num_points());
            if (calibration_is_valid())
            {
                uart_send_str("  slope=");
                uart_send_float3(calibration_get_slope());
                uart_send_str("  offset=");
                uart_send_float3(calibration_get_offset());
            }
            uart_send_str("\r\n");
        }
        else
        {
            /* CAL <value>[ PSI] — capture at given reference pressure */
            float bar    = 0.0f;
            bool  in_psi = false;
            bool  num_ok = parse_pressure_bar(arg, &bar, &in_psi);

            if (capturing)
            {
                uart_send_str("ERR: capture in progress, wait for 'Captured'\r\n");
            }
            else if (calibration_get_state() != CAL_ARMED)
            {
                uart_send_str("ERR: CAL ARM first\r\n");
            }
            else if (calibration_get_num_points() >= CAL_MAX_POINTS)
            {
                /* Without this, the capture request is silently dropped and
                 * the operator waits forever for a "Captured" line.        */
                uart_send_str("ERR: max 8 pts (STORE or ABORT)\r\n");
            }
            else if (num_ok && (bar > 0.0f) && (bar <= RANGE_BAR_CEIL))
            {
                calibration_capture(bar);
                uart_send_str("Capturing at ");
                uart_send_float3(bar);
                uart_send_str(" bar");
                if (in_psi)
                {
                    uart_send_str(" (");
                    uart_send_float3(bar * PSI_PER_BAR);
                    uart_send_str(" psi)");
                }
                uart_send_str("...\r\n");
            }
            else
            {
                uart_send_str("ERR: CAL <0..1000 bar>[ PSI]|ARM|STORE|CLEAR|STATUS|ABORT\r\n");
            }
        }
    }
    else if (cmd_prefix(cmd, "PSI "))
    {
        /* Unit converter: gauge reads psi, firmware works in bar. */
        const char *arg = cmd + 4;
        if ((*arg >= '0' && *arg <= '9') || *arg == '.')
        {
            float v = parse_float(arg);
            uart_send_float3(v);
            uart_send_str(" psi = ");
            uart_send_float3(v * BAR_PER_PSI);
            uart_send_str(" bar\r\n");
        }
        else
        {
            uart_send_str("ERR: PSI <value>\r\n");
        }
    }
    else if (cmd_prefix(cmd, "BAR "))
    {
        const char *arg = cmd + 4;
        if ((*arg >= '0' && *arg <= '9') || *arg == '.')
        {
            float v = parse_float(arg);
            uart_send_float3(v);
            uart_send_str(" bar = ");
            uart_send_float3(v * PSI_PER_BAR);
            uart_send_str(" psi\r\n");
        }
        else
        {
            uart_send_str("ERR: BAR <value>\r\n");
        }
    }
    else if (cmd_eq(cmd, "POWER"))
    {
        uart_send_str("Power: ");
        uart_send_u16(POWER_CONTINUOUS_MW);
        uart_send_str(" mW (continuous)\r\n");
    }
    else if (cmd_eq(cmd, "HELP"))
    {
        uart_send_str("Commands:\r\n");
        uart_send_str("  STATUS          — probe counts + fault\r\n");
        uart_send_str("  RAW             — one burst: avg/min/max/mV/valid per ch\r\n");
        uart_send_str("  SCAN            — sweep all analog inputs (find live pin)\r\n");
        uart_send_str("  AUTO            — toggle auto-print\r\n");
        uart_send_str("  RATE <ms>       — set refresh rate (NVM)\r\n");
        uart_send_str("  THRESH <cnt>    — disagree threshold (NVM)\r\n");
        uart_send_str("  RANGE <lo> <hi> — output window, bar (NVM)\r\n");
        uart_send_str("  PROBE A|B|AVG   — probe source (NVM)\r\n");
        uart_send_str("  OUTPUT <n>/AUTO — manual output / resume\r\n");
        uart_send_str("  POWER           — power consumption\r\n");
        uart_send_str("  CAL ARM         — start calibration\r\n");
        uart_send_str("  CAL <bar>       — capture at pressure (max 8 pts)\r\n");
        uart_send_str("  CAL <x> PSI     — same, value in psi\r\n");
        uart_send_str("  CAL STORE       — compute + save NVM\r\n");
        uart_send_str("  CAL CLEAR       — erase calibration\r\n");
        uart_send_str("  CAL ABORT       — abort cal session\r\n");
        uart_send_str("  CAL STATUS      — show cal coefficients\r\n");
        uart_send_str("  PSI <x>/BAR <x> — unit converter\r\n");
        uart_send_str("  HELP            — this list\r\n");
    }
    /* Bare keywords: print the command's usage instead of "unknown". */
    else if (cmd_eq(cmd, "RATE"))   { uart_send_str("ERR: RATE <100-10000 ms>\r\n"); }
    else if (cmd_eq(cmd, "THRESH")) { uart_send_str("ERR: THRESH <1-1023>\r\n"); }
    else if (cmd_eq(cmd, "RANGE"))  { uart_send_str("ERR: RANGE <lo> <hi> [PSI]  (0<=lo<hi<=1000 bar)\r\n"); }
    else if (cmd_eq(cmd, "PROBE"))  { uart_send_str("ERR: PROBE A|B|AVG\r\n"); }
    else if (cmd_eq(cmd, "OUTPUT")) { uart_send_str("ERR: OUTPUT <0-1023>|AUTO\r\n"); }
    else if (cmd_eq(cmd, "CAL"))    { uart_send_str("ERR: CAL <bar>[ PSI]|ARM|STORE|CLEAR|STATUS|ABORT\r\n"); }
    else if (cmd_eq(cmd, "PSI"))    { uart_send_str("ERR: PSI <value>\r\n"); }
    else if (cmd_eq(cmd, "BAR"))    { uart_send_str("ERR: BAR <value>\r\n"); }
    else
    {
        uart_send_str("ERR: unknown '");
        uart_send_str(cmd);
        uart_send_str("' (try HELP)\r\n");
    }
}
