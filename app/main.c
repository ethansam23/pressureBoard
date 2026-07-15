/* ===========================================================================
 * EXPERIMENTAL BENCH-ONLY FIRMWARE — branch exp/adc-scope. NEVER DEPLOY.
 *
 * Turns the board into a diagnostic ADC scope: raw single 10-bit conversions
 * (NO oversampling) streamed on UART2/P1.0 at ADC_SCOPE_BAUD (1 Mbaud,
 * ~50 kS/s), plus a RAM burst capture at full polled-ADC speed (~µs
 * resolution). P1.0 carries THIS raw stream, not the logger protocol — the
 * production modules (link_tx/link_frame/uart_cmd/calibration/fault/nvm)
 * still compile but are never called: their timing is 9600-hardcoded.
 *
 * Wire format (stream), 2 bytes per sample, self-syncing:
 *   b0 = 0x80 | ch<<6 | seq2<<4 | sample[9:6]      (bit7 = 1: frame start)
 *   b1 = sample[5:0]                               (bit7 = 0)
 *   ch: 0 = Probe A (AN7/P2.7), 1 = Probe B (AN3/P2.3); seq2 = rolling
 *   2-bit counter for drop detection.
 * Host commands (single bytes): 'A'/'B' switch channel, 'T' burst capture.
 * Burst dump: 0xF0 0x0F, count u16 LE, duration_us u16 LE, count × sample
 * (lo, hi — hi ≤ 0x07), then an 8-bit additive checksum over count/duration/
 * payload bytes. Invalid conversions are stored as 0x0400.
 *
 * WDT1 law unchanged: SystemInit issued the first trigger; servicing only
 * via scheduler_service()/WDT1_Service() — every loop state honors the
 * ~300 ms budget.
 * ========================================================================= */
#include "tle_device.h"
#include "app_config.h"
#include "scheduler.h"

#if LINK_CONSOLE_EN
#error "exp/adc-scope requires LINK_CONSOLE_EN=0 (RAM for the burst buffer)"
#endif

static uint16 burst_buf[ADC_SCOPE_BURST_N];

/* ---- one raw conversion (mirrors acquisition.c sample_one) --------------- */
static bool convert(uint8 ch, uint16 *out)
{
    uint16 val   = 0u;
    uint32 guard = ADC_EOC_TIMEOUT_SPINS;

    ADC1_SetSosSwMode(ch);
    while (ADC1_GetEocSwMode() == false)
    {
        if (guard == 0u) { return false; }   /* bounded — never spin forever */
        guard--;
    }
    if (ADC1_GetChResult(&val, ch) == false) { return false; }
    *out = val;
    return true;
}

/* Mux-settle throwaway — needed once after every channel switch only. */
static void channel_select(uint8 ch)
{
    uint16 dummy;
    (void)convert(ch, &dummy);
}

/* ---- polled TX (vendor stdout_putchar pattern; ints stay disabled) ------- */
static void tx_byte(uint8 b)
{
    UART2_Send_Byte(b);
    while (UART2_isByteTransmitted() == false) { }
}

/* ---- burst: capture at full ADC speed, then dump slowly ------------------ */
static void burst_run(uint8 ch)
{
    uint32 tpu = (SysTick->LOAD + 1u) / 1000u;    /* SysTick ticks per µs     */
    uint32 ms0, ms1, st0, st1, ticks, us;
    uint16 i;
    uint8  sum = 0u;

    (void)WDT1_Service();

    ms0 = scheduler_get_ms();
    st0 = SysTick->VAL;                            /* counts DOWN             */
    for (i = 0u; i < (uint16)ADC_SCOPE_BURST_N; i++)
    {
        uint16 v;
        if (convert(ch, &v) == false) { v = 0x0400u; }   /* invalid marker    */
        burst_buf[i] = v;
    }
    ms1 = scheduler_get_ms();
    st1 = SysTick->VAL;

    (void)WDT1_Service();

    /* Elapsed µs from ms counter + SysTick phase (modular math is exact). */
    ticks = ((ms1 - ms0) * (SysTick->LOAD + 1u)) + st0 - st1;
    us    = ticks / tpu;
    if (us > 0xFFFFu) { us = 0xFFFFu; }

    tx_byte(0xF0u);
    tx_byte(0x0Fu);
    {
        uint8 hdr[4];
        hdr[0] = (uint8)(ADC_SCOPE_BURST_N & 0xFFu);
        hdr[1] = (uint8)((ADC_SCOPE_BURST_N >> 8) & 0xFFu);
        hdr[2] = (uint8)(us & 0xFFu);
        hdr[3] = (uint8)((us >> 8) & 0xFFu);
        for (i = 0u; i < 4u; i++) { tx_byte(hdr[i]); sum = (uint8)(sum + hdr[i]); }
    }
    for (i = 0u; i < (uint16)ADC_SCOPE_BURST_N; i++)
    {
        uint8 lo = (uint8)(burst_buf[i] & 0xFFu);
        uint8 hi = (uint8)((burst_buf[i] >> 8) & 0xFFu);
        (void)scheduler_service();                 /* WDT during the ~26 ms dump */
        tx_byte(lo); sum = (uint8)(sum + lo);
        tx_byte(hi); sum = (uint8)(sum + hi);
    }
    tx_byte(sum);
}

int main(void)
{
    uint8  ch     = ADC_CH_PROBE_A;
    uint8  ch_bit = 0u;
    uint8  seq    = 0u;
    uint32 led_ctr = 0u;
    bool   led_on  = false;

    /* SystemInit() already issued the FIRST WDT1 trigger and started
     * SysTick — do NOT call WDT1_Init() or trigger WDT1 here (a second
     * trigger in the closed window resets; 5 resets latch Sleep Mode).      */
    TLE_Init();
    scheduler_init();

    PORT_P04_Output_Set();                         /* status LED, push-pull   */
    PORT_P04_Output_Low_Set();

    /* VDDEXT excitation for the bridges — BOUNDED wait, same rule as the
     * production firmware: never spin forever on a rail that may be latched
     * off. On timeout, stream anyway (the data itself shows the sag).       */
    {
        uint32 t0 = scheduler_get_ms();
        while (PMU_VDDEXT_On() == false)
        {
            (void)WDT1_Service();
            if ((scheduler_get_ms() - t0) >= VDDEXT_SETTLE_TIMEOUT_MS)
            {
                break;
            }
        }
    }

    /* UART2 on P1.0 (alt 3) — polled I/O only, module interrupts off.
     * Pin/mode setup mirrors link_tx_init(); baud is the experiment's.      */
    PORT_P10_Output_Set();
    PORT_ChangePinAlt(0x10u, 3u);
    PORT_P11_PullUp_Set();                         /* RX idle-high            */
    PORT_P11_PullUpDown_En();
    UART2->SCON.reg |= (uint32)((1u << 6u) | (1u << 4u));  /* SM1 + REN       */
    UART2_BaudRate_Set(ADC_SCOPE_BAUD);
    UART2_TX_Int_Dis();
    UART2_RX_Int_Dis();
    UART2_RX_Int_Clr();

    ADC1_Software_Mode_Sel();
    channel_select(ch);

    /* ---- stream loop: TX-paced at 2 bytes/sample ------------------------- */
    for (;;)
    {
        uint16 sample;
        uint8  b0, b1;

        (void)scheduler_service();                 /* WDT1, every pass        */

        if (UART2_RX_Sts() == 1u)                  /* 1-byte host commands    */
        {
            uint8 c = UART2_Get_Byte();
            UART2_RX_Int_Clr();
            if ((c == (uint8)'A') || (c == (uint8)'a'))
            {
                ch = ADC_CH_PROBE_A; ch_bit = 0u; channel_select(ch);
            }
            else if ((c == (uint8)'B') || (c == (uint8)'b'))
            {
                ch = ADC_CH_PROBE_B; ch_bit = 1u; channel_select(ch);
            }
            else if ((c == (uint8)'T') || (c == (uint8)'t'))
            {
                burst_run(ch);
            }
            else
            {
                /* unknown byte: ignore                                       */
            }
        }

        if (convert(ch, &sample) == false)
        {
            continue;                              /* skip; next loop retries */
        }

        b0 = (uint8)(0x80u | ((uint32)ch_bit << 6) | (((uint32)seq & 3u) << 4)
                     | ((sample >> 6) & 0x0Fu));
        b1 = (uint8)(sample & 0x3Fu);
        seq++;
        tx_byte(b0);                               /* ~20 µs/sample: the wire */
        tx_byte(b1);                               /* paces the whole loop    */

        led_ctr++;
        if (led_ctr >= ADC_SCOPE_LED_DIV)
        {
            led_ctr = 0u;
            led_on  = !led_on;
            if (led_on) { PORT_P04_Output_High_Set(); }
            else        { PORT_P04_Output_Low_Set();  }
        }
    }
}
