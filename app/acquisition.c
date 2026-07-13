#include "acquisition.h"
#include "app_config.h"
#include "nvm_config.h"
#include "adc1.h"

/* Last known-good conversion per SDK channel (indices 0-15 cover CH6-CH12).
 * Substituted when a conversion times out or the result-valid flag is clear,
 * so a single bad conversion can't drag the 16-sample average toward 0. */
static uint16 last_good[16];

/* Fallbacks (EOC timeout / invalid result) in the current acquisition_run —
 * lets the caller detect a dead/stalled ADC instead of silently emitting
 * last_good (or 0 on a fresh boot, which would read as a valid 0.5 V). */
static uint16 run_fallbacks;

void acquisition_init(void)
{
    ADC1_Software_Mode_Sel();
}

static uint16 sample_one(uint8 ch)
{
    uint16 val   = 0u;
    uint32 guard = ADC_EOC_TIMEOUT_SPINS;

    ADC1_SetSosSwMode(ch);                    /* select channel + start SOC   */

    /* BOUNDED wait: normally EOC lands in a few µs at 40 MHz. Never spin
     * forever -- a stalled conversion would starve WDT1, and 5 watchdog
     * resets latch the chip into Sleep Mode.                                */
    while (ADC1_GetEocSwMode() == false)
    {
        guard--;
        if (guard == 0u) { run_fallbacks++; return last_good[ch & 0x0Fu]; }
    }

    /* The result register's valid flag (VF) gates the read: on VF=0 the SDK
     * leaves val untouched, so fall back to the last good sample instead of
     * folding a bogus 0 into the average.                                   */
    if (ADC1_GetChResult(&val, ch) == false)
    {
        run_fallbacks++;
        return last_good[ch & 0x0Fu];
    }
    last_good[ch & 0x0Fu] = val;
    return val;
}

/* Mux-settle throwaway: start + discard one conversion WITHOUT touching
 * last_good (this is exactly the sample we consider unsettled) or the
 * fallback accounting. */
static void sample_discard(uint8 ch)
{
    uint16 val   = 0u;
    uint32 guard = ADC_EOC_TIMEOUT_SPINS;

    ADC1_SetSosSwMode(ch);
    while (ADC1_GetEocSwMode() == false)
    {
        guard--;
        if (guard == 0u) { return; }
    }
    (void)ADC1_GetChResult(&val, ch);
}

static uint16 oversample(uint8 ch)
{
    uint32 sum = 0u;
    uint8  i;

    sample_discard(ch);     /* let the S/H settle after the mux switch       */

    for (i = 0u; i < OVERSAMPLE_COUNT; i++)
    {
        sum += sample_one(ch);
    }
    return (uint16)(sum / (uint32)OVERSAMPLE_COUNT);
}

void acquisition_run(acq_result_t *result)
{
    run_fallbacks = 0u;

    result->probe_a = oversample(ADC_CH_PROBE_A);
    result->probe_b = oversample(ADC_CH_PROBE_B);

    /* A full channel's worth of fallbacks means the ADC is effectively dead
     * this cycle -- flag it so main drives the fault band instead of a
     * plausible-looking stale/zero reading. */
    result->stalled = (run_fallbacks >= (uint16)OVERSAMPLE_COUNT);

    /* The "combined" value feeds the output + calibration; pick its source
     * per the configured probe mode (single-probe boards use A or B). */
    switch (nvm_config_get_probe_mode())
    {
    case PROBE_MODE_A:
        result->combined = result->probe_a;
        break;
    case PROBE_MODE_B:
        result->combined = result->probe_b;
        break;
    default:
        result->combined =
            (uint16)(((uint32)result->probe_a + (uint32)result->probe_b) / 2u);
        break;
    }
}

/* ---- Diagnostics --------------------------------------------------------- */
/* Single conversion that reports the result-register valid flag (VF) to the
 * caller (production sample_one() substitutes last_good on VF=0; here the
 * raw behavior is wanted so RAW/SCAN can count validity). */
static uint16 sample_raw(uint8 ch, bool *valid)
{
    uint16 val   = 0u;
    uint32 guard = ADC_EOC_TIMEOUT_SPINS;

    ADC1_SetSosSwMode(ch);
    while (ADC1_GetEocSwMode() == false)
    {
        guard--;
        if (guard == 0u) { *valid = false; return 0u; }   /* bounded, like sample_one */
    }
    *valid = ADC1_GetChResult(&val, ch);
    return val;
}

static void debug_channel(uint8 ch, acq_chan_debug_t *d)
{
    uint32 sum = 0u;
    uint32 fs;
    uint8  i;
    bool   v;
    uint16 raw;

    d->min   = 0xFFFFu;
    d->max   = 0u;
    d->valid = 0u;

    /* One throwaway conversion to let the S/H settle on the freshly-selected
     * channel (mux just switched from the other probe). */
    (void)sample_raw(ch, &v);

    for (i = 0u; i < OVERSAMPLE_COUNT; i++)
    {
        raw = sample_raw(ch, &v);
        if (v) { d->valid++; }
        sum += (uint32)raw;
        if (raw < d->min) { d->min = raw; }
        if (raw > d->max) { d->max = raw; }
    }

    d->avg = (uint16)(sum / (uint32)OVERSAMPLE_COUNT);

    /* Convert avg counts -> mV using the SDK's per-channel attenuation factor
     * (same maths as ADC1_GetChResult_mV). */
    fs = ADC1_GetChAttFactor(ch);
    if (d->avg > 0u)
    {
        d->mv = (uint16)((((uint32)d->avg * fs) - (fs >> 1u)) >> 10u);
    }
    else
    {
        d->mv = 0u;
    }
}

void acquisition_debug(acq_debug_t *dbg)
{
    debug_channel(ADC_CH_PROBE_A, &dbg->a);
    debug_channel(ADC_CH_PROBE_B, &dbg->b);
}

void acquisition_scan_channel(uint8 ch, acq_chan_debug_t *out)
{
    debug_channel(ch, out);
}

uint16 acquisition_counts_to_mv(uint16 counts)
{
    /* Both probe channels are P2.x inputs (same attenuation group). */
    uint32 fs = ADC1_GetChAttFactor(ADC_CH_PROBE_A);
    if (counts == 0u) { return 0u; }
    return (uint16)((((uint32)counts * fs) - (fs >> 1u)) >> 10u);
}
