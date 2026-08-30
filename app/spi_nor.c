#include "spi_nor.h"
#include "app_config.h"
#include "port.h"
#include "scu.h"
#include "scheduler.h"
#include "wdt1.h"

/* ---- Driver state -------------------------------------------------------- */
static uint8 jedec_id[3];
static bool  present;
static uint8 phase_used;
static bool  phase_flipped;

/* Set whenever a byte transfer timed out waiting for receive-complete —
 * i.e. the shifter is not running at all (clock gate, enable, or pin mux),
 * as opposed to running fine and reading a quiet line. Sticky until the
 * next probe so the console can report which of the two happened.         */
static bool  last_timeout;

/* ========================================================================= */
/*  SSC1 low level                                                           */
/* ========================================================================= */

/* Build the CON word for 8-bit MSB-first SPI master at the given clock
 * phase. EN is left clear — the caller sets it last, because the data-format
 * bits must not change while the shifter is enabled.
 *
 * BM is the data width MINUS ONE (the datasheet advertises "2 to 16 bits" and
 * BM is 4 bits wide, so 8 data bits == 7). HB=1 selects MSB-first, which is
 * what every SPI NOR expects. PO=0 idles the clock low. AREN=1 lets the
 * hardware clear its own error flags rather than latching a stale one.      */
static uint32 con_word(uint8 ph)
{
    return ((uint32)7u                  << SSC1_CON_BM_Pos)   |  /* 8 bits    */
           ((uint32)1u                  << SSC1_CON_HB_Pos)   |  /* MSB first */
           ((uint32)(ph & 1u)           << SSC1_CON_PH_Pos)   |
           ((uint32)0u                  << SSC1_CON_PO_Pos)   |  /* idle low  */
           ((uint32)1u                  << SSC1_CON_AREN_Pos) |
           ((uint32)1u                  << SSC1_CON_MS_Pos);     /* master    */
}

/* Reconfigure the shifter for a clock phase. Disable, rewrite the format,
 * re-enable — changing PH under an enabled shifter is not defined behaviour. */
static void ssc1_set_phase(uint8 ph)
{
    SSC1->CON.reg = 0u;                       /* EN=0: format bits writable  */
    SSC1->BR.reg  = (uint32)NOR_SSC1_BR;
    SSC1->CON.reg = con_word(ph);
    SSC1->CON.reg = con_word(ph) | ((uint32)1u << SSC1_CON_EN_Pos);
    phase_used    = (uint8)(ph & 1u);
}

/* One byte out, one byte in — SPI is always full duplex, so a "read" is a
 * write of a don't-care byte.
 *
 * Completion is detected with the SSC1 RECEIVE flag (SCU->IRCON2.RIR1), not
 * with CON.BSY. Polling BSY straight after writing TB is a race: BSY does
 * not assert in the same cycle as the write, so the first read can still see
 * it clear, the loop exits immediately, and RB is read before it was ever
 * loaded — yielding 0x00 (its reset value) on every single byte. RIR1 is set
 * by hardware only when a byte has actually been shifted in, so waiting on
 * it cannot complete early. The flag is cleared before the byte and again
 * after consuming it, so a stale request can never satisfy the next wait.
 *
 * BOUNDED, never unbounded: a hung shifter must not take the super-loop with
 * it (hard constraint — downhole the packet stream is the only signal). A
 * timeout returns 0xFF and is recorded in `last_timeout`, because "the
 * shifter never ran" and "the flash answered 0xFF" are completely different
 * faults and must not look alike from the console.
 *
 * Blocking is acceptable here ONLY because every caller in this file moves a
 * handful of bytes: 4 bytes at 1 MHz is ~32 us, against a 9.2 ms packet and a
 * ~300 ms WDT1 budget. Page programs and erases must not use this.          */
static uint8 ssc1_xfer(uint8 out)
{
    uint32 guard = NOR_SPIN_GUARD;

    SCU->IRCON2CLR.reg = (uint32)SCU_IRCON2CLR_RIR1C_Msk;
    SSC1->TB.reg       = (uint32)out;

    while ((SCU->IRCON2.reg & (uint32)SCU_IRCON2_RIR1_Msk) == 0u)
    {
        if (guard == 0u)
        {
            last_timeout = true;
            return 0xFFu;
        }
        guard--;
    }

    SCU->IRCON2CLR.reg = (uint32)SCU_IRCON2CLR_RIR1C_Msk;

    return (uint8)(SSC1->RB.reg & 0xFFu);
}

/* CE# is active low and is a plain GPIO — the SSC has no automatic chip
 * select. Held asserted across a whole command, and later across multiple
 * super-loop passes once the byte pump lands.                              */
static void ce_assert(void)   { PORT_P12_Output_Low_Set();  }
static void ce_release(void)  { PORT_P12_Output_High_Set(); }

/* ========================================================================= */
/*  Public                                                                   */
/* ========================================================================= */

void spi_nor_init(void)
{
    jedec_id[0]   = 0u;
    jedec_id[1]   = 0u;
    jedec_id[2]   = 0u;
    present       = false;
    phase_used    = (uint8)NOR_SSC1_PH;
    phase_flipped = false;
    last_timeout  = false;

    /* Module clock. The RTE has no SSC driver, but the SCU helper is generic
     * and does gate SSC1 (scu.h, Mod_SSC1).                                 */
    SCU_Enable_Module(Mod_SSC1);

    /* CE# first and inactive, BEFORE the bus pins become SSC outputs — a
     * floating CE# while SCK starts toggling would clock garbage into the
     * flash's command decoder.                                              */
    PORT_P12_Output_Set();
    ce_release();

    /* SCK (P0.3) and SI (P0.4) are SSC1 outputs via ALT1. SO (P0.5) stays an
     * input, which is its reset state; SSC1_M_MRST is an INPUT function
     * (INP1) and is selected through PISEL, not through the ALT mux.        */
    PORT_P03_Output_Set();
    PORT_ChangePinAlt((uint32)PIN_NOR_SCK, (uint8)NOR_ALT_SSC1);
    PORT_P04_Output_Set();
    PORT_ChangePinAlt((uint32)PIN_NOR_SI,  (uint8)NOR_ALT_SSC1);

    /* SO is an input. Whether it gets an internal pull depends entirely on
     * which translator it runs through — see NOR_SO_PULLUP in app_config.h.
     * On an auto-direction part the pull must be OFF or it can defeat the
     * direction sensing; on a unidirectional channel it is a useful
     * diagnostic. Never decide this here.                                  */
    PORT_P05_Input_Set();
#if (NOR_SO_PULLUP != 0u)
    PORT_P05_PullUp_Set();
    PORT_P05_PullUpDown_En();
#else
    PORT_P05_PullUpDown_Dis();
#endif

    /* PISEL = 0 selects the dedicated SSC1_M_MRST mapping on P0.5. The other
     * MIS encodings reach the shared SSC12_M_MRST_x inputs on P0.2 / P2.7,
     * neither of which is wired to the flash (and P2.7 is Probe A). If SO
     * ever reads as all-ones with a known-good phase, these bits are the
     * next thing to question.                                               */
    SSC1->PISEL.reg = 0u;

    ssc1_set_phase((uint8)NOR_SSC1_PH);
}

/* Read the 3-byte JEDEC ID into `out`. */
static void read_id(uint8 out[3])
{
    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_RDID);
    out[0] = ssc1_xfer(0x00u);       /* manufacturer */
    out[1] = ssc1_xfer(0x00u);       /* memory type  */
    out[2] = ssc1_xfer(0x00u);       /* capacity     */
    ce_release();
}

static bool id_matches(const uint8 id[3])
{
    return (id[0] == (uint8)NOR_ID_MANUFACTURER) &&
           (id[1] == (uint8)NOR_ID_TYPE) &&
           (id[2] == (uint8)NOR_ID_CAPACITY);
}

bool spi_nor_probe(void)
{
    uint8 alt;

    phase_flipped = false;
    last_timeout  = false;

    read_id(jedec_id);
    if (id_matches(jedec_id))
    {
        present = true;
        return true;
    }

    /* The configured phase did not answer. The PH bit's semantics are not
     * documented in the local datasheet, so before blaming the wiring, try
     * the other phase — and remember that we had to, so the bench sees it
     * and app_config.h can be corrected.                                    */
    alt = (uint8)(((uint8)NOR_SSC1_PH ^ 1u) & 1u);
    ssc1_set_phase(alt);

    read_id(jedec_id);
    if (id_matches(jedec_id))
    {
        present       = true;
        phase_flipped = true;
        return true;
    }

    /* Neither phase answered. Restore the configured one so the reported
     * state matches app_config.h, and leave the raw ID for the console:
     * all-00 or all-FF points at wiring, power, or the level translators
     * rather than at anything in this file.                                 */
    ssc1_set_phase((uint8)NOR_SSC1_PH);
    present = false;
    return false;
}

void spi_nor_get_id(uint8 out[3])
{
    out[0] = jedec_id[0];
    out[1] = jedec_id[1];
    out[2] = jedec_id[2];
}

bool  spi_nor_present(void)          { return present; }
uint8 spi_nor_phase_used(void)       { return phase_used; }
bool  spi_nor_phase_was_flipped(void){ return phase_flipped; }
bool  spi_nor_bus_timed_out(void)    { return last_timeout; }

uint8 spi_nor_read_status(void)
{
    uint8 sr;

    if (!present) { return 0xFFu; }

    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_RDSR);
    sr = ssc1_xfer(0x00u);
    ce_release();

    return sr;
}

/* ========================================================================= */
/*  Bench-only blocking operations (see the header's BENCH USE ONLY note)    */
/* ========================================================================= */

/* 24-bit address, MSB first — the IS25LP128F's 3-byte addressing mode
 * reaches the whole 16 MB, so no 4-byte mode is needed.                     */
static void send_addr24(uint32 addr)
{
    (void)ssc1_xfer((uint8)((addr >> 16) & 0xFFu));
    (void)ssc1_xfer((uint8)((addr >>  8) & 0xFFu));
    (void)ssc1_xfer((uint8)(addr & 0xFFu));
}

/* A write-enable latch is armed per operation and is cleared by the device
 * when that operation completes — so this must precede every erase and every
 * program, not just the first.                                             */
static void write_enable(void)
{
    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_WREN);
    ce_release();
}

bool spi_nor_wait_ready(uint32 timeout_ms)
{
    uint32 t0 = scheduler_get_ms();

    if (!present) { return false; }

    for (;;)
    {
        if ((spi_nor_read_status() & (uint8)NOR_SR_WIP) == 0u)
        {
            return true;
        }

        /* Window-aware: WDT1_Service() only triggers once the window is
         * open, which is why the fence loops can call it in a tight spin
         * too. A sector erase can outlast the ~300 ms budget, so this is
         * not optional.                                                    */
        (void)WDT1_Service();

        if ((scheduler_get_ms() - t0) >= timeout_ms)
        {
            return false;      /* time-boxed — never spin forever on a bit */
        }
    }
}

bool spi_nor_erase_sector(uint32 addr)
{
    if (!present) { return false; }

    write_enable();

    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_SER);
    send_addr24(addr);
    ce_release();              /* the erase starts when CE# rises           */

    return spi_nor_wait_ready((uint32)NOR_WAIT_SE_MS);
}

bool spi_nor_write_page(uint32 addr, const uint8 *data, uint16 len)
{
    uint16 i;

    if (!present || (data == 0) || (len == 0u) || (len > (uint16)NOR_PAGE_SIZE))
    {
        return false;
    }

    /* A program that runs past the page boundary wraps to the START of the
     * same page instead of continuing — it would silently overwrite data
     * already written there. Reject rather than truncate.                  */
    if ((((addr & ((uint32)NOR_PAGE_SIZE - 1u)) + (uint32)len)) >
        (uint32)NOR_PAGE_SIZE)
    {
        return false;
    }

    write_enable();

    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_PP);
    send_addr24(addr);
    for (i = 0u; i < len; i++)
    {
        (void)ssc1_xfer(data[i]);
    }
    ce_release();              /* the program starts when CE# rises         */

    return spi_nor_wait_ready((uint32)NOR_WAIT_PP_MS);
}

/* ========================================================================= */
/*  Self-test                                                                */
/* ========================================================================= */

/* Internal loopback: CON.LB ties the shifter's output back to its own input,
 * so a byte written to TB must reappear in RB with no external hardware
 * involved at all.
 *
 * This is the clean split between "my driver is wrong" and "the board is not
 * passing signals". It exercises the clock gate, the enable bit, the baud
 * reload, the data width, and the RIR1/RB completion handling — everything
 * except the pin mux and the outside world. If loopback passes and a real
 * transfer still reads 0xFF, the fault is provably external.
 *
 * Note what it CANNOT test: bit order cancels out (the same shifter unwinds
 * what it wound), and the ALT mux is bypassed. A wrong data width does show
 * up, because the byte comes back truncated.
 *
 * CE# is left deasserted throughout, so the flash ignores any edges that do
 * reach the pins.                                                          */
bool spi_nor_loopback(uint8 got[4])
{
    static const uint8 patterns[4] = { 0x55u, 0xAAu, 0x01u, 0x80u };
    uint32 saved_con = SSC1->CON.reg;
    uint8  i;
    bool   ok = true;

    ce_release();

    /* Re-enable with LB set. Format bits are only writable while disabled,
     * hence the drop to zero first.                                        */
    SSC1->CON.reg = 0u;
    SSC1->BR.reg  = (uint32)NOR_SSC1_BR;
    SSC1->CON.reg = con_word(phase_used) | ((uint32)1u << SSC1_CON_LB_Pos);
    SSC1->CON.reg = con_word(phase_used) | ((uint32)1u << SSC1_CON_LB_Pos)
                                         | ((uint32)1u << SSC1_CON_EN_Pos);

    for (i = 0u; i < 4u; i++)
    {
        got[i] = ssc1_xfer(patterns[i]);
        if (got[i] != patterns[i]) { ok = false; }
    }

    /* Restore exactly what was running before.                             */
    SSC1->CON.reg = 0u;
    SSC1->BR.reg  = (uint32)NOR_SSC1_BR;
    SSC1->CON.reg = saved_con & ~((uint32)1u << SSC1_CON_EN_Pos);
    SSC1->CON.reg = saved_con | ((uint32)1u << SSC1_CON_EN_Pos);

    return ok;
}

/* ========================================================================= */
/*  Pin-level bring-up diagnostics                                           */
/* ========================================================================= */

void spi_nor_diag_begin(void)
{
    /* Take SCK and SI back from SSC1 (AltSel 0 = plain GPIO output) so they
     * can be held at a DC level. CE# is already a GPIO.                     */
    PORT_ChangePinAlt((uint32)PIN_NOR_SCK, 0u);
    PORT_ChangePinAlt((uint32)PIN_NOR_SI,  0u);
    PORT_P03_Output_Set();
    PORT_P04_Output_Set();

    /* Park everything in its inactive state: CE# high (deselected), clock
     * idle low to match PO=0, data low.                                     */
    PORT_P12_Output_High_Set();
    PORT_P03_Output_Low_Set();
    PORT_P04_Output_Low_Set();
}

void spi_nor_diag_set(uint8 sig, bool high)
{
    switch (sig)
    {
    case (uint8)NOR_SIG_CE:
        if (high) { PORT_P12_Output_High_Set(); } else { PORT_P12_Output_Low_Set(); }
        break;
    case (uint8)NOR_SIG_SCK:
        if (high) { PORT_P03_Output_High_Set(); } else { PORT_P03_Output_Low_Set(); }
        break;
    case (uint8)NOR_SIG_SI:
        if (high) { PORT_P04_Output_High_Set(); } else { PORT_P04_Output_Low_Set(); }
        break;
    default:
        break;
    }
}

uint8 spi_nor_diag_so(bool pull_down)
{
    uint8 lvl;

    if (pull_down) { PORT_P05_PullDown_Set(); }
    else           { PORT_P05_PullUp_Set();   }
    PORT_P05_PullUpDown_En();

    /* Let the pull win against pin capacitance before sampling. An internal
     * pull is weak (tens of kilo-ohms), so this settles in microseconds —
     * but not instantly, and reading too early would report the previous
     * state and invert the whole diagnosis.                                */
    {
        volatile uint32 s = 2000u;
        while (s != 0u) { s--; }
    }

    lvl = PORT_P05_Get();

    /* Leave SO in whatever state the driver actually runs in — never a
     * stray pull left behind. On an auto-direction translator a forgotten
     * pull-down here would quietly defeat the direction sensing for every
     * subsequent transfer, and the fault would look nothing like its cause. */
#if (NOR_SO_PULLUP != 0u)
    PORT_P05_PullUp_Set();
    PORT_P05_PullUpDown_En();
#else
    PORT_P05_PullUpDown_Dis();
#endif

    return (uint8)(lvl != 0u ? 1u : 0u);
}

/* Half-bit delay for the bit-banged probe. Deliberately glacial (~50 us, so
 * ~10 kHz) — slow enough that translator one-shots, trace capacitance and
 * any marginal edge rate are all irrelevant. If the wiring works at all, it
 * works at this speed.                                                     */
static void bb_delay(void)
{
    volatile uint32 n = 300u;
    while (n != 0u) { n--; }
}

/* Bit-banged JEDEC ID read on plain GPIO — SPI mode 0, MSB first.
 *
 * This exists to bracket the fault. LOG LOOP tests SSC1 with no wiring
 * involved; this tests the wiring with no SSC1 involved. Between them there
 * is nowhere left to hide:
 *
 *   loopback PASS + bitbang PASS  -> the ALT pin mux is the problem
 *   loopback PASS + bitbang FAIL  -> wiring, translator, or the flash
 *   bitbang PASS + SSC1 FAIL      -> SSC1 config (phase, PISEL, baud)
 *
 * It bypasses the SSC entirely: the peripheral, the ALT mux, the baud
 * generator, PISEL and the PH hypothesis are all out of the path. Only the
 * pads, the translators, the traces and the flash remain.                  */
bool spi_nor_bitbang_id(uint8 out[3])
{
    uint8 i, b;

    spi_nor_diag_begin();          /* pins -> plain GPIO, CE# high, SCK low */

    bb_delay();
    spi_nor_diag_set((uint8)NOR_SIG_CE, false);    /* select                */
    bb_delay();

    for (b = 0u; b < 4u; b++)
    {
        /* Byte 0 is the RDID opcode; the next three clock the ID back. */
        uint8 tx = (b == 0u) ? (uint8)NOR_CMD_RDID : 0x00u;
        uint8 rx = 0u;

        for (i = 0u; i < 8u; i++)
        {
            /* Mode 0: data is presented while the clock is low and the
             * device samples it on the rising edge; we likewise sample SO
             * on the rising edge, MSB first.                             */
            spi_nor_diag_set((uint8)NOR_SIG_SI, ((tx & 0x80u) != 0u));
            tx = (uint8)(tx << 1);
            bb_delay();

            spi_nor_diag_set((uint8)NOR_SIG_SCK, true);
            rx = (uint8)(rx << 1);
            if (PORT_P05_Get() != 0u) { rx |= 1u; }
            bb_delay();

            spi_nor_diag_set((uint8)NOR_SIG_SCK, false);
        }

        if (b > 0u) { out[b - 1u] = rx; }
    }

    bb_delay();
    spi_nor_diag_set((uint8)NOR_SIG_CE, true);     /* deselect              */
    bb_delay();

    spi_nor_diag_end();            /* full SSC1 re-init                     */

    return id_matches(out);
}

void spi_nor_diag_end(void)
{
    /* Full re-init: restores the ALT mux, PISEL, baud, phase and CE# state,
     * so the bus is left exactly as normal operation expects. Presence is
     * re-established by the caller running a probe.                        */
    spi_nor_init();
}

bool spi_nor_read(uint32 addr, uint8 *out, uint16 len)
{
    uint16 i;

    if (!present || (out == 0) || (len == 0u)) { return false; }

    ce_assert();
    (void)ssc1_xfer((uint8)NOR_CMD_READ);
    send_addr24(addr);
    for (i = 0u; i < len; i++)
    {
        out[i] = ssc1_xfer(0x00u);
    }
    ce_release();

    return true;
}
