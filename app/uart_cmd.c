#include "uart_cmd.h"
#include "app_config.h"
#include "scheduler.h"
#include "link_tx.h"
#include "link_frame.h"
#include "calibration.h"
#include "nvm_config.h"
#include "fault.h"
#include "acquisition.h"
#include "uart.h"
#include "port.h"
#include "wdt1.h"

/*******************************************************************************
 * Bench console — debug builds only (LINK_CONSOLE_EN).
 *
 * UART2 is OWNED by the downhole link (link_tx.c). The console shares it
 * under strict mutual exclusion:
 *   - Boots LOCKED: no TX text, all commands ignored, until the exact line
 *     "CONSOLE UNLOCK" arrives on RX. The stream stays pure packets.
 *   - UNLOCK suspends the packet stream (after the in-flight packet
 *     completes atomically) and gives the console the line.
 *   - "CONSOLE LOCK", a 5-min RX-inactivity timeout, or a power cycle
 *     re-locks: the console ring drains, the line goes idle for a full
 *     inter-packet gap, then the stream resumes.
 * Production builds (LINK_CONSOLE_EN=0) compile the console out entirely:
 * uart_send_* become no-ops, RX is never enabled — packets are the only
 * bytes this firmware can ever emit.
 ******************************************************************************/

/* ---- Ring buffers ------------------------------------------------------- */
static volatile uint8  tx_buf[UART_TX_BUF_SIZE];
static volatile uint16 tx_head, tx_tail;

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
#if LINK_CONSOLE_EN
static bool   console_unlocked;   /* boot-locked; exact UNLOCK line opens it  */
static bool   banner_pending;     /* banner prints once the line is granted   */
static uint32 last_rx_tick;       /* inactivity auto-relock reference         */
static uint32 boot_rst, boot_wfs; /* reset cause captured at boot by main     */
#endif

/* ---- Forward declarations ----------------------------------------------- */
static void uart_putc(uint8 c);
#if LINK_CONSOLE_EN
static void process_cmd(const char *cmd);
static bool cmd_eq(const char *a, const char *b);
static bool cmd_prefix(const char *cmd, const char *prefix);
static uint16 parse_u16(const char *s);
static float  parse_float(const char *s);
static void console_relock(void);
static void print_banner(void);
#endif

/* ========================================================================= */
/*  Public API                                                               */
/* ========================================================================= */
void uart_cmd_init(void)
{
    tx_head = 0u;  tx_tail = 0u;
    rx_head = 0u;  rx_tail = 0u;
    cmd_len = 0u;
    auto_print = false;

#if LINK_CONSOLE_EN
    console_unlocked = false;      /* BOOTS LOCKED — stream stays pure        */
    banner_pending   = false;
    last_rx_tick     = scheduler_get_ms();
    boot_rst = 0u; boot_wfs = 0u;

    /* UART2 pins / mode / baud / NVIC are owned by link_tx_init() (which
     * runs first). The console only adds the RECEIVER + RX interrupt.       */
    UART2_Receiver_En();

    /* Drain any spurious byte latched while the Rx line settled, and clear
     * the RX flag, so the FIRST real command isn't corrupted by garbage.    */
    (void)UART2_Buffer_Get();
    UART2_RX_Int_Clr();
    UART2_RX_Int_En();
#endif
}

void uart_cmd_set_boot_info(uint32 rst, uint32 wfs)
{
#if LINK_CONSOLE_EN
    boot_rst = rst;
    boot_wfs = wfs;
#else
    (void)rst; (void)wfs;
#endif
}

void uart_cmd_service(void)
{
#if LINK_CONSOLE_EN
    uint32 now = scheduler_get_ms();

    /* Deferred unlock banner: the link reports when the in-flight packet
     * has completed and the console owns the line.                          */
    if (banner_pending && link_tx_console_active())
    {
        banner_pending = false;
        print_banner();
    }

    /* Inactivity auto-relock: covers a crashed host app, an unplugged
     * adapter, or a forgotten bench session — the board re-arms the
     * packet stream on its own.                                             */
    if (console_unlocked && !banner_pending &&
        ((now - last_rx_tick) >= LINK_CONSOLE_RELOCK_MS))
    {
        console_relock();
    }

    /* Keep the console ring draining while we own the line.                 */
    if (link_tx_console_active()) { link_tx_console_kick(); }

    /* --- Pull bytes from RX ring buffer into command line buffer --------- */
    while (rx_head != rx_tail)
    {
        uint8 b = rx_buf[rx_tail];
        rx_tail = (rx_tail + 1u) % UART_RX_BUF_SIZE;
        last_rx_tick = now;

        if (b == '\r' || b == '\n')
        {
            /* Process on Enter. Blank lines (and the LF of a CR-LF pair,
             * since cmd_len is already 0) are ignored without echoing.      */
            if (cmd_len > 0u)
            {
                /* Trim trailing spaces so "STATUS " matches STATUS.         */
                while (cmd_len > 0u && cmd_buf[cmd_len - 1u] == (uint8)' ')
                {
                    cmd_len--;
                }
                cmd_buf[cmd_len] = '\0';

                if (!console_unlocked)
                {
                    /* LOCKED: exactly one line is recognized; everything
                     * else is silently ignored (no echo, no error — the
                     * line must stay packet-pure).                          */
                    if (cmd_eq((const char *)cmd_buf, "CONSOLE UNLOCK"))
                    {
                        console_unlocked = true;
                        banner_pending   = true;
                        link_tx_request_console();
                    }
                }
                else
                {
                    uart_send_str("\r\n");
                    process_cmd((const char *)cmd_buf);
                }
                cmd_len = 0u;
                /* At most ONE command per service pass: a pasted batch of
                 * RAW/SCAN lines would otherwise run hundreds of conversions
                 * between WDT1 services. Remaining bytes wait one pass.     */
                break;
            }
        }
        else if (b == 0x08u || b == 0x7Fu)      /* backspace / delete */
        {
            if (cmd_len > 0u)
            {
                cmd_len--;
                uart_send_str("\b \b");         /* no-op while locked */
            }
        }
        else if (b >= 0x20u && b <= 0x7Eu)      /* printable ASCII only */
        {
            if (cmd_len < (UART_CMD_BUF_SIZE - 1u))
            {
                cmd_buf[cmd_len++] = b;
                uart_putc(b);                   /* echo (no-op while locked) */
            }
            /* else: line full -> drop extra chars silently */
        }
        /* else: control / line-noise byte -> ignore                         */
    }
#endif
}

void uart_cmd_update_readings(uint16 probe_a, uint16 probe_b, uint16 combined)
{
    rd_probe_a  = probe_a;
    rd_probe_b  = probe_b;
    rd_combined = combined;

#if LINK_CONSOLE_EN
    if (auto_print && console_unlocked)
    {
        /* Debug stream: counts + mV per probe, pressure (if cal'd), and the
         * link code that WOULD be on the wire (stream is suspended while
         * the console is unlocked) plus which path produced it.             */
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

        uart_send_str("  Link:0x");
        uart_send_hex16(link_tx_get_live_code());
        uart_send_str(" ");
        if (link_tx_is_test())           { uart_send_str("TST"); }
        else if (fault_is_active())      { uart_send_str("FLT"); }
        else if (calibration_is_valid()) { uart_send_str("CAL"); }
        else                             { uart_send_str("UNC"); }
        uart_send_str("\r\n");
    }
#endif
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
#if LINK_CONSOLE_EN
    uint8  b    = UART2_Buffer_Get();
    uint16 next = (rx_head + 1u) % UART_RX_BUF_SIZE;
    if (next != rx_tail)            /* room in buffer */
    {
        rx_buf[rx_head] = b;
        rx_head = next;
    }
    /* overflow: drop byte */
#endif
}

void uart_cmd_tx_isr(void)
{
    /* isr_defines.h (vendor config, untouched) wires the UART2 TX callback
     * to this name. The link module is the single TX arbiter — packet
     * bursts always win; console bytes flow only in bench console mode.    */
    link_tx_tx_isr();
}

/* ISR/kick-context console-ring accessors for the link arbiter.             */
bool uart_cmd_console_pop(uint8 *b)
{
#if LINK_CONSOLE_EN
    if (tx_head == tx_tail) { return false; }
    *b = tx_buf[tx_tail];
    tx_tail = (tx_tail + 1u) % UART_TX_BUF_SIZE;
    return true;
#else
    (void)b;
    return false;
#endif
}

bool uart_cmd_console_ring_empty(void)
{
    return (tx_head == tx_tail);
}

/* ========================================================================= */
/*  Internal helpers                                                         */
/* ========================================================================= */
static void uart_putc(uint8 c)
{
#if LINK_CONSOLE_EN
    uint16 next;
    if (!console_unlocked) { return; }  /* locked: the line stays packet-pure */
    next = (tx_head + 1u) % UART_TX_BUF_SIZE;
    if (next == tx_tail) { return; }    /* buffer full — drop                 */
    tx_buf[tx_head] = c;
    tx_head = next;
    link_tx_console_kick();             /* primes hardware iff console owns
                                         * the line and it is idle           */
#else
    (void)c;                            /* production: console TX impossible  */
#endif
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

void uart_send_u32(uint32 val)
{
    char  buf[10];
    uint8 i = 0u;
    if (val == 0u) { uart_putc('0'); return; }
    while (val > 0u)
    {
        buf[i++] = (char)('0' + (uint8)(val % 10u));
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
#if LINK_CONSOLE_EN
    uint32 t0 = scheduler_get_ms();
    if (!link_tx_console_active()) { return; }  /* nothing drains in pkt mode */
    while (tx_head != tx_tail)
    {
        link_tx_console_kick();
        (void)WDT1_Service();
        if ((scheduler_get_ms() - t0) >= 1500u) { break; }  /* 1KB @960B/s    */
    }
#endif
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

#if LINK_CONSOLE_EN

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

static void print_banner(void)
{
    uart_send_str("\r\n== Pressure Transmitter v2 (digital link) ==\r\n");
    uart_send_str("Console UNLOCKED — packet stream SUSPENDED (CONSOLE LOCK to resume)\r\n");
    uart_send_str("Boot RST 0x"); uart_send_hex16((uint16)boot_rst);
    uart_send_str(" WFS 0x");     uart_send_hex16((uint16)boot_wfs);
    if ((boot_rst & PMU_RESET_STS_PMU_ExtWDT_Msk) != 0u) { uart_send_str(" [WDT1]"); }
    if ((boot_rst & PMU_RESET_STS_PMU_VS_POR_Msk) != 0u) { uart_send_str(" [POR]");  }
    if ((boot_rst & PMU_RESET_STS_PMU_PIN_Msk)    != 0u) { uart_send_str(" [PIN]");  }
    uart_send_str("\r\n");
    if (!nvm_flash_is_healthy())
    {
        uart_send_str("WARN: NVM data flash inconsistent (saves disabled)\r\n");
    }
}

static void console_relock(void)
{
    /* Goodbye line is queued while still unlocked; it drains during the
     * link's RESUMING phase, then the line goes quiet and packets resume.  */
    uart_send_str("Console LOCKED — packet stream resuming\r\n");
    console_unlocked = false;
    auto_print       = false;    /* a forgotten AUTO must not re-arm later   */
    link_tx_request_packet();
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

    uart_send_str("Link: 0x");   uart_send_hex16(link_tx_get_live_code());
    uart_send_str(link_tx_is_test() ? "  TEST(!)" : "  LIVE");
    uart_send_str("  mode=");
    uart_send_str(link_tx_console_active() ? "CONSOLE (stream suspended)" : "PKT");
    uart_send_str("  pkts=");    uart_send_u32(link_tx_get_pkt_count());
    uart_send_str(" aborts=");   uart_send_u16(link_tx_get_abort_total());
    uart_send_str(" skips=");    uart_send_u16(link_tx_get_busy_skips());
    uart_send_str("\r\n");

    uart_send_str("Faults: ");
    if (!fault_is_active()) { uart_send_str("none"); }
    else
    {
        if (fault_adc_active())      { uart_send_str("ADC_STALL "); }
        if (fault_vddext_active())   { uart_send_str("VDDEXT ");    }
        if (fault_disagree_active()) { uart_send_str("DISAGREE");   }
    }
    uart_send_str("\r\n");

    uart_send_str("Rate: ");       uart_send_u16((uint16)scheduler_get_rate_ms());
    uart_send_str("ms  Thresh: "); uart_send_u16(nvm_config_get_disagree_thresh());
    uart_send_str("  NVM: ");      uart_send_str(nvm_flash_is_healthy() ? "ok" : "INCONSISTENT");
    uart_send_str("\r\n");

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
    uart_send_str("RAW (native 10-bit, 1 LSB ~5mV; production counts = 4x):\r\n");
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
        /* ~34 ms of conversions — safe here: commands only run in console
         * mode, where the packet stream is suspended (nothing to tear).    */
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
    else if (cmd_eq(cmd, "CONSOLE LOCK"))
    {
        console_relock();
    }
    else if (cmd_eq(cmd, "CONSOLE UNLOCK"))
    {
        uart_send_str("Already unlocked\r\n");
    }
    else if (cmd_prefix(cmd, "LINKTEST "))
    {
        const char *arg = cmd + 9;
        if (cmd_eq(arg, "OFF"))
        {
            link_tx_clear_test();
            uart_send_str("LinkTest OFF — live values resume\r\n");
        }
        else if (is_clean_u16(arg))
        {
            uint16 c = parse_u16(arg);
            link_tx_set_test(c);
            uart_send_str("LinkTest=0x");
            uart_send_hex16(c);
            uart_send_str(" — OVERRIDES live/fault codes; auto-expires in 5 min\r\n");
            uart_send_str("(stream is suspended while unlocked: CONSOLE LOCK to transmit it)\r\n");
        }
        else
        {
            uart_send_str("ERR: LINKTEST <0-65535>|OFF\r\n");
        }
    }
    else if (cmd_prefix(cmd, "THRESH "))
    {
        uint16 t = parse_u16(cmd + 7);
        if (is_clean_u16(cmd + 7) && t > 0u && t <= ADC_COUNTS_MAX)
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
            uart_send_str("ERR: thresh 1-4092\r\n");
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
            uart_send_str("ERR: rate 100-5000\r\n");
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
            else if (num_ok && (bar > 0.0f) && (bar <= SENSOR_RATING_BAR))
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
        uart_send_str("  STATUS          — readings, link state, faults\r\n");
        uart_send_str("  RAW             — one burst: avg/min/max/mV/valid per ch\r\n");
        uart_send_str("  SCAN            — sweep all analog inputs (find live pin)\r\n");
        uart_send_str("  AUTO            — toggle auto-print\r\n");
        uart_send_str("  RATE <ms>       — set refresh rate (NVM)\r\n");
        uart_send_str("  THRESH <cnt>    — disagree threshold (NVM)\r\n");
        uart_send_str("  PROBE A|B|AVG   — probe source (NVM)\r\n");
        uart_send_str("  LINKTEST <n>|OFF— force a 16-bit wire code (5 min)\r\n");
        uart_send_str("  CONSOLE LOCK    — re-lock console, resume stream\r\n");
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
    else if (cmd_eq(cmd, "RATE"))     { uart_send_str("ERR: RATE <100-5000 ms>\r\n"); }
    else if (cmd_eq(cmd, "THRESH"))   { uart_send_str("ERR: THRESH <1-4092>\r\n"); }
    else if (cmd_eq(cmd, "PROBE"))    { uart_send_str("ERR: PROBE A|B|AVG\r\n"); }
    else if (cmd_eq(cmd, "LINKTEST")) { uart_send_str("ERR: LINKTEST <0-65535>|OFF\r\n"); }
    else if (cmd_eq(cmd, "CONSOLE"))  { uart_send_str("ERR: CONSOLE LOCK|UNLOCK\r\n"); }
    else if (cmd_eq(cmd, "CAL"))      { uart_send_str("ERR: CAL <bar>[ PSI]|ARM|STORE|CLEAR|STATUS|ABORT\r\n"); }
    else if (cmd_eq(cmd, "PSI"))      { uart_send_str("ERR: PSI <value>\r\n"); }
    else if (cmd_eq(cmd, "BAR"))      { uart_send_str("ERR: BAR <value>\r\n"); }
    else
    {
        uart_send_str("ERR: unknown '");
        uart_send_str(cmd);
        uart_send_str("' (try HELP)\r\n");
    }
}

#endif /* LINK_CONSOLE_EN */
