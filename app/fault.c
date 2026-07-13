#include "fault.h"
#include "nvm_config.h"

static bool active;       /* probe disagreement (auto-clearing)              */
static bool sys_fault;    /* excitation/system fault (set by supervision)    */

void fault_init(void)
{
    active    = false;
    sys_fault = false;
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
     * the analog line between fault-low and the live value. */
    band = (uint16)(thresh >> 3);
    if (band == 0u) { band = 1u; }

    if (diff > thresh)
    {
        active = true;
    }
    else if ((diff + band) <= thresh)
    {
        active = false;
    }
    /* else: inside the deadband — keep the previous state */
}

bool fault_is_active(void)
{
    return (active || sys_fault);
}

void fault_clear(void)
{
    active = false;
}

void fault_raise_system(void)
{
    sys_fault = true;
}

void fault_clear_system(void)
{
    sys_fault = false;
}
