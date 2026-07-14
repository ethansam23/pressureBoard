#include "link_tx.h"
#include "link_frame.h"
#include "app_config.h"
#include "scheduler.h"
#include "uart_cmd.h"
#include "uart.h"
#include "port.h"
#include "wdt1.h"

/* ---- Packet engine state -------------------------------------------------- */
static link_sm_t sm;

/* Burst buffer handed to the TX ISR. The state machine guarantees at most
 * one burst in flight; the ISR chains the bytes back-to-back so the 3-byte
 * data block never depends on main-loop latency.                             */
static volatile uint8  burst[3];
static volatile uint8  burst_len;
static volatile uint8  burst_idx;
static volatile bool   burst_active;

/* Global UART-TX hardware state (Concern: SM==IDLE does not mean the UART is
 * idle — a console byte may be mid-shift). Set at EVERY hardware load (packet
 * or console), cleared at that byte's TI. Owned by the ISR.                  */
static volatile bool   in_flight;
static volatile uint32 last_ti_tick;   /* TI tick of the most recent byte     */

/* Consume-once completion event for the state machine (sync byte done /
 * final burst byte done). Written in ISR, consumed in link_tx_service().     */
static volatile bool   evt_done;
static volatile uint32 evt_done_tick;

/* ---- Mode / fence / codes ------------------------------------------------- */
typedef enum
{
    MODE_PACKET = 0,    /* stream owns the line (only mode in production)     */
    MODE_SUSPENDING,    /* unlock requested: finish the in-flight packet      */
    MODE_CONSOLE,       /* console owns the line; packet engine parked        */
    MODE_RESUMING       /* lock requested: drain console, idle, then resume   */
} link_mode_t;

static volatile link_mode_t mode;

static bool   held;                /* fence hold: no NEW packet may start     */
static uint16 live_code;
static uint16 last_sent_code;
static bool   test_on;
static uint16 test_code;
static uint32 test_t0;

/* ========================================================================= */
/*  Init                                                                     */
/* ========================================================================= */
void link_tx_init(void)
{
    /* The link owns UART2. Base setup here so production builds (console
     * compiled out) are self-sufficient; uart_cmd_init() adds RX on top in
     * debug builds. P1.0 = UART2 TXD (alt 3): push-pull ~5V when driven,
     * HIGH-Z during MCU reset — the idle-high pull during reset is the
     * HARNESS's job (external pull-up; see link_protocol.md, gate Q16/17). */
    PORT_P10_Output_Set();
    PORT_ChangePinAlt(0x10u, 3u);

    /* P1.1 (console RX, unused downhole) floats without this: pull it to
     * the UART idle level so line noise cannot form characters.             */
    PORT_P11_PullUp_Set();
    PORT_P11_PullUpDown_En();

    /* Config Wizard leaves SCON = 0; SM1 = 1 selects Mode 1 (8-bit UART,
     * variable baud) — same fix uart_cmd_init applied at 115200.            */
    UART2->SCON.reg |= (uint32)(1u << 6u);
    UART2_BaudRate_Set(UART_BAUD);             /* 9600 — the logger's rate    */

    /* UART2 NVIC node (IRQ 11). The TX module interrupt is enabled only
     * while a transmission is active.                                       */
    CPU->NVIC_ISER.reg |= (1u << 11u);

    burst_active = false;
    in_flight    = false;
    evt_done     = false;
    last_ti_tick = 0u;
    mode         = MODE_PACKET;
    held         = false;
    test_on      = false;
    live_code    = (uint16)LINK_CODE_NO_READING;
    last_sent_code = (uint16)LINK_CODE_NO_READING;

    /* Stream is alive from this moment: first sync fires on the first
     * service pass, carrying NO_READING until the first sample lands.       */
    link_sm_init(&sm, scheduler_get_ms());
}

/* ========================================================================= */
/*  Burst start (service context only)                                       */
/* ========================================================================= */
static void start_burst(const uint8 *b, uint8 n)
{
    uint8 i;
    __disable_irq();
    if (burst_active || in_flight)
    {
        /* Stale byte / overlapping burst — must never happen (the SM gates
         * on uart_idle). Conservative recovery rather than corrupting the
         * wire.                                                             */
        __enable_irq();
        link_sm_force_recovery(&sm, LINK_ABT_EXTERNAL, scheduler_get_ms());
        return;
    }
    for (i = 0u; i < n; i++) { burst[i] = b[i]; }
    burst_len    = n;
    burst_idx    = 1u;
    burst_active = true;
    in_flight    = true;
    UART2_Buffer_Set(b[0]);        /* prime; ISR chains the rest              */
    UART2_TX_Int_En();
    __enable_irq();
}

/* ========================================================================= */
/*  Service (main loop, every iteration)                                     */
/* ========================================================================= */
void link_tx_service(void)
{
    uint32        now = scheduler_get_ms();
    link_sm_in_t  in;
    link_action_t act;

    /* LINKTEST auto-expiry: a forced test code must not be able to ride
     * into a deployment log because someone forgot to clear it.             */
    if (test_on && ((now - test_t0) >= LINKTEST_EXPIRY_MS))
    {
        test_on = false;
    }

    /* Mode transitions (mutual exclusion with the bench console).           */
    if ((mode == MODE_SUSPENDING) && !link_sm_in_packet(&sm) && !in_flight)
    {
        mode = MODE_CONSOLE;       /* in-flight packet completed atomically   */
    }
    if ((mode == MODE_RESUMING) && !in_flight && uart_cmd_console_ring_empty() &&
        ((now - last_ti_tick) >= (LINK_IDLE_MIN_MS + 1u)))
    {
        /* Console text was the last wire activity: base the SM's idle
         * arithmetic on the true last byte before handing the line back.    */
        sm.t_quiet_ref = last_ti_tick;
        mode = MODE_PACKET;
    }

    in.now         = now;
    in.live_code   = test_on ? test_code : live_code;
    in.uart_idle   = !in_flight;
    in.held        = held;
    in.packet_mode = (mode == MODE_PACKET);
    __disable_irq();
    in.tx_done      = evt_done;
    in.tx_done_tick = evt_done_tick;
    evt_done        = false;
    __enable_irq();

    act = link_sm_step(&sm, &in);
    if (act == LINK_ACT_SEND_SYNC)
    {
        uint8 sync = (uint8)LINK_SYNC_BYTE;
        last_sent_code = sm.code;
        start_burst(&sync, 1u);
    }
    else if (act == LINK_ACT_SEND_DATA)
    {
        uint8 d[3];
        d[0] = sm.data[0]; d[1] = sm.data[1]; d[2] = sm.data[2];
        start_burst(d, 3u);
    }
}

/* ========================================================================= */
/*  TX ISR — the single UART2 TX arbiter                                     */
/* ========================================================================= */
void link_tx_tx_isr(void)
{
    uint32 tick = scheduler_get_ms();
    last_ti_tick = tick;

    if (burst_active)
    {
        if (burst_idx < burst_len)
        {
            UART2_Buffer_Set(burst[burst_idx]);   /* chain back-to-back       */
            burst_idx++;
            return;                               /* in_flight stays true     */
        }
        burst_active  = false;
        evt_done      = true;                     /* sync or data block done  */
        evt_done_tick = tick;
    }
    in_flight = false;

    /* Console bytes: ONLY while the console owns the line (or is draining
     * on the way out). In PACKET mode the console cannot reach the wire.    */
    if ((mode == MODE_CONSOLE) || (mode == MODE_RESUMING))
    {
        uint8 b;
        if (uart_cmd_console_pop(&b))
        {
            in_flight = true;
            UART2_Buffer_Set(b);
            return;
        }
    }
    UART2_TX_Int_Dis();
}

void link_tx_console_kick(void)
{
    uint8 b;
    if ((mode != MODE_CONSOLE) && (mode != MODE_RESUMING)) { return; }
    __disable_irq();
    if (!in_flight && uart_cmd_console_pop(&b))
    {
        in_flight = true;
        UART2_Buffer_Set(b);
        UART2_TX_Int_En();
    }
    __enable_irq();
}

/* ========================================================================= */
/*  Fence                                                                    */
/* ========================================================================= */
bool link_tx_fence_bounded(void)
{
    uint32 t0 = scheduler_get_ms();
    held = true;
    for (;;)
    {
        uint32 now;
        link_tx_service();      /* keep consuming TI events / stepping SM     */
        now = scheduler_get_ms();
        if (!link_sm_in_packet(&sm) && !burst_active && !in_flight &&
            ((now - last_ti_tick) >= 2u))   /* >=1 tick past TI: shift-reg
                                             * allowance (TI hypothesis)      */
        {
            return true;        /* hold stays set; caller must release()      */
        }
        (void)WDT1_Service();
        if ((now - t0) >= LINK_FENCE_TIMEOUT_MS)
        {
            held = false;       /* fail-closed: no hold left behind           */
            return false;
        }
    }
}

void link_tx_release(void)
{
    /* Transmits NOTHING. The next packet starts on a later service pass
     * once the period deadline + idle-min allow — one packet, rebased on
     * its actual sync start, never a catch-up burst.                        */
    held = false;
}

/* ========================================================================= */
/*  Mode requests (uart_cmd drives these on UNLOCK/LOCK)                     */
/* ========================================================================= */
void link_tx_request_console(void)
{
    if (mode == MODE_PACKET) { mode = MODE_SUSPENDING; }
}

void link_tx_request_packet(void)
{
    if ((mode == MODE_CONSOLE) || (mode == MODE_SUSPENDING))
    {
        mode = MODE_RESUMING;
    }
}

bool link_tx_console_active(void)
{
    return (mode == MODE_CONSOLE);
}

/* ========================================================================= */
/*  Codes / counters                                                         */
/* ========================================================================= */
void   link_tx_set_live_code(uint16 code) { live_code = code; }
uint16 link_tx_get_live_code(void)        { return test_on ? test_code : live_code; }
uint16 link_tx_get_last_code(void)        { return last_sent_code; }

void link_tx_set_test(uint16 code)
{
    test_code = code;
    test_t0   = scheduler_get_ms();
    test_on   = true;
}
void link_tx_clear_test(void) { test_on = false; }
bool link_tx_is_test(void)    { return test_on; }

uint32 link_tx_get_pkt_count(void) { return sm.pkts_ok; }

uint16 link_tx_get_abort_total(void)
{
    uint32 sum = 0u;
    uint8  i;
    for (i = 0u; i < (uint8)LINK_ABT_COUNT; i++) { sum += sm.aborts[i]; }
    return (sum > 0xFFFFu) ? 0xFFFFu : (uint16)sum;
}

uint16 link_tx_get_busy_skips(void) { return sm.busy_skips; }
