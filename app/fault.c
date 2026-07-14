#include "fault.h"
#include "nvm_config.h"

static bool disagree;     /* probe disagreement (auto-clearing)              */
static bool adc_stall;    /* ADC conversions dead (set by supervision)       */
static bool vddext_bad;   /* excitation rail unstable (set by supervision)   */

void fault_init(void)
{
    disagree   = false;
    adc_stall  = false;
    vddext_bad = false;
}

void fault_check(uint16 probe_a, uint16 probe_b)
{
    uint16 diff;
    uint16 thresh = nvm_config_get_disagree_thresh();
    uint16 band;

    if (probe_a >= probe_b)
    {
        diff = probe_a - probe_b;
    }
    else
    {
        diff = probe_b - probe_a;
    }

    /* Hysteresis (~thresh/8, min 1 count): a reading hovering right at the
     * threshold must not flap the fault every refresh — that square-waves
     * the output between the fault code and the live value. */
    band = (uint16)(thresh >> 3);
    if (band == 0u) { band = 1u; }

    if (diff > thresh)
    {
        disagree = true;
    }
    else if ((diff + band) <= thresh)
    {
        disagree = false;
    }
    /* else: inside the deadband — keep the previous state */
}

bool fault_is_active(void)
{
    return (disagree || adc_stall || vddext_bad);
}

void fault_clear(void)
{
    disagree = false;
}

void fault_raise_adc(void)     { adc_stall  = true;  }
void fault_clear_adc(void)     { adc_stall  = false; }
void fault_raise_vddext(void)  { vddext_bad = true;  }
void fault_clear_vddext(void)  { vddext_bad = false; }

bool fault_disagree_active(void) { return disagree;   }
bool fault_adc_active(void)      { return adc_stall;  }
bool fault_vddext_active(void)   { return vddext_bad; }
