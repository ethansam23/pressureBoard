#include "link_frame.h"
#include <math.h>   /* isfinite, floorf — softfloat on M0, main-loop only    */
#include <stddef.h> /* NULL — standard C, keeps this file SDK-free          */

/* All tick comparisons use the wrap-safe unsigned idiom (now - t0) >= X.    */

uint8_t link_checksum(uint8_t msb, uint8_t lsb)
{
    return (uint8_t)(~(uint8_t)(msb + lsb));
}

uint16_t link_encode_bar(float bar)
{
    /* Order matters: NaN fails every comparison below, so it MUST be caught
     * first — a non-finite float reaching the uint16 conversion is undefined
     * behavior. Non-finite means the stored calibration is unusable.        */
    if (!isfinite(bar))
    {
        return (uint16_t)LINK_CODE_UNCAL;
    }
    if (bar < LINK_UNDER_LIMIT_BAR)
    {
        return (uint16_t)LINK_CODE_UNDER_RANGE;
    }
    if (bar < 0.0f)
    {
        return 0u;                       /* [-5, 0) clamps to zero            */
    }
    if (bar > LINK_OVER_LIMIT_BAR)
    {
        return (uint16_t)LINK_CODE_OVER_RANGE;
    }
    if (bar > LINK_FULL_SCALE_BAR)
    {
        return (uint16_t)LINK_VALUE_MAX; /* (1000, 1010] clamps to full scale */
    }
    /* Round-half-up. bar is in [0, 1000] here, so the argument is in
     * [0.5, 10000.5] and the result fits uint16 with room to spare.         */
    return (uint16_t)floorf((bar * 10.0f) + 0.5f);
}

void link_build_data(uint16_t code, uint8_t out[3])
{
    out[0] = (uint8_t)(code >> 8);       /* big-endian: MSB first             */
    out[1] = (uint8_t)(code & 0xFFu);
    out[2] = link_checksum(out[0], out[1]);
}

/* ---- State machine -------------------------------------------------------- */

void link_sm_init(link_sm_t *sm, uint32_t now)
{
    uint8_t i;
    sm->st           = LINK_ST_IDLE;
    /* Pre-age the references so the first sync fires on the first service
     * pass: the stream must be alive from power-on (boot fail-safe).        */
    sm->t_prev_sync  = now - LINK_PACKET_PERIOD_MS;
    sm->t_quiet_ref  = now - (LINK_IDLE_MIN_MS + 1u);
    sm->t_sync_start = sm->t_prev_sync;
    sm->t_last_ti    = sm->t_quiet_ref;
    sm->code         = (uint16_t)LINK_CODE_NO_READING;
    sm->pkts_ok      = 0u;
    sm->busy_skips   = 0u;
    for (i = 0u; i < (uint8_t)LINK_ABT_COUNT; i++) { sm->aborts[i] = 0u; }
    link_build_data(sm->code, sm->data);
}

static void abort_to_recovery(link_sm_t *sm, link_abort_t origin, uint32_t now)
{
    if (sm->aborts[origin] < 0xFFFFu) { sm->aborts[origin]++; }
    /* Wire state is uncertain — take 'now' as the last-activity reference
     * and sit out a FULL idle-min before anything else goes out.            */
    sm->t_quiet_ref = now;
    sm->st          = LINK_ST_RECOVERY;
}

void link_sm_force_recovery(link_sm_t *sm, link_abort_t origin, uint32_t now)
{
    abort_to_recovery(sm, origin, now);
}

bool link_sm_in_packet(const link_sm_t *sm)
{
    return (sm->st == LINK_ST_SYNC) ||
           (sm->st == LINK_ST_GAP)  ||
           (sm->st == LINK_ST_DATA);
}

link_action_t link_sm_step(link_sm_t *sm, const link_sm_in_t *in)
{
    link_action_t act = LINK_ACT_NONE;

    switch (sm->st)
    {
    case LINK_ST_IDLE:
        /* One packet may start iff: period deadline passed AND the wire has
         * been quiet for idle-min (+1 tick TI allowance) AND no hold AND the
         * console doesn't own the line AND the UART is physically idle.
         * The period rebases on the ACTUAL sync start — an overdue packet
         * fires once at the first legal opportunity; never a catch-up burst. */
        if (((in->now - sm->t_prev_sync) >= LINK_PACKET_PERIOD_MS) &&
            ((in->now - sm->t_quiet_ref) >= (LINK_IDLE_MIN_MS + 1u)) &&
            (!in->held) && in->packet_mode)
        {
            if (!in->uart_idle)
            {
                if (sm->busy_skips < 0xFFFFu) { sm->busy_skips++; }
            }
            else
            {
                sm->code         = in->live_code;      /* single latch point  */
                link_build_data(sm->code, sm->data);
                sm->t_sync_start = in->now;
                sm->t_prev_sync  = in->now;
                sm->st           = LINK_ST_SYNC;
                act              = LINK_ACT_SEND_SYNC;
            }
        }
        break;

    case LINK_ST_SYNC:
        if (in->tx_done)
        {
            sm->t_last_ti = in->tx_done_tick;
            sm->st        = LINK_ST_GAP;
        }
        else if ((in->now - sm->t_sync_start) >= LINK_SYNC_TIMEOUT_MS)
        {
            /* TI never came — UART state unknown.                           */
            abort_to_recovery(sm, LINK_ABT_SYNC_TIMEOUT, in->now);
        }
        break;

    case LINK_ST_GAP:
        if ((in->now - sm->t_last_ti) >= LINK_SYNC_GAP_MS)
        {
            if ((in->now - sm->t_sync_start) <= LINK_DATA_DEADLINE_MS)
            {
                sm->st = LINK_ST_DATA;
                act    = LINK_ACT_SEND_DATA;
            }
            else
            {
                /* An unpredicted stall ate the packet window. Only the sync
                 * byte is on the wire; abandon the packet rather than emit a
                 * data block that would overrun the receiver's 10ms limit.  */
                abort_to_recovery(sm, LINK_ABT_DATA_DEADLINE, in->now);
            }
        }
        break;

    case LINK_ST_DATA:
        if (in->tx_done)
        {
            sm->t_last_ti   = in->tx_done_tick;
            sm->t_quiet_ref = in->tx_done_tick;
            sm->pkts_ok++;
            sm->st = LINK_ST_IDLE;
        }
        else if ((in->now - sm->t_sync_start) >=
                 (LINK_DATA_DEADLINE_MS + LINK_DATA_TIMEOUT_MS))
        {
            abort_to_recovery(sm, LINK_ABT_DATA_TIMEOUT, in->now);
        }
        break;

    case LINK_ST_RECOVERY:
        /* Conservative quiet-down: full idle-min measured from the abort
         * instant, plus confirmed hardware idle, before IDLE may consider a
         * fresh sync.                                                        */
        if (((in->now - sm->t_quiet_ref) >= (LINK_IDLE_MIN_MS + 1u)) &&
            in->uart_idle)
        {
            sm->st = LINK_ST_IDLE;
        }
        break;

    default:
        abort_to_recovery(sm, LINK_ABT_BAD_STATE, in->now);
        break;
    }

    return act;
}

/* ===========================================================================
 *  Bench simulation profile — see the header for the phase/tier rationale.
 *  Integer-only: never introduce a float here. host_tests/ compiles this
 *  under gcc so the host reference stream comes from this exact code.
 * ======================================================================== */
#if APP_ENABLE_SIM

typedef struct
{
    uint8_t  kind;       /* SIM_SEG_*                                        */
    uint16_t target;     /* RAMP: end code. HOLD: held code. STATUS: unused  */
    uint32_t dur_ms;     /* wall-clock duration                              */
} sim_seg_t;

/* ---- Phase A — resolution sweep (20,600 s) --------------------------------
 * The ladder in Phase B skips codes on every ramp, so this is the only thing
 * that proves all 10,001 deci-bar codes encode and transmit. One code per
 * refresh at RATE 1000; slower RATE values just hold each code longer.      */
static const sim_seg_t sim_tab_a[] =
{
    { SIM_SEG_STATUS,     0u,       35000u },
    { SIM_SEG_HOLD,       0u,       60000u },
    { SIM_SEG_RAMP,   10000u,    10000000u },   /* 0 -> full scale, 1 dbar/rf */
    { SIM_SEG_HOLD,   10000u,      300000u },   /* dwell at the cap           */
    { SIM_SEG_RAMP,       0u,    10000000u },   /* full scale -> 0            */
    { SIM_SEG_HOLD,       0u,      205000u },
};

/* ---- Phase B — ramp-timing ladder (3,600 s per cycle) ---------------------
 * Five tiers of FIXED-DURATION windows; the slew rate is whatever the delta
 * over that window requires. Rates below are dbar/refresh at RATE 1000 and
 * are all exact integers there. Window durations never change, so retargeting
 * a ramp changes only its rate — the cycle stays exactly 3,600,000 ms.      */
static const sim_seg_t sim_tab_b[] =
{
    { SIM_SEG_STATUS,     0u,       35000u },
    /* tier 1 — 5 min windows: rates 1, 5, 14, -20                           */
    { SIM_SEG_RAMP,     300u,      300000u }, { SIM_SEG_HOLD,   300u, 120000u },
    { SIM_SEG_RAMP,    1800u,      300000u }, { SIM_SEG_HOLD,  1800u, 120000u },
    { SIM_SEG_RAMP,    6000u,      300000u }, { SIM_SEG_HOLD,  6000u, 120000u },
    { SIM_SEG_RAMP,       0u,      300000u }, { SIM_SEG_HOLD,     0u, 120000u },
    /* tier 2 — 2 min windows: rates 2, 8, 40, -50                           */
    { SIM_SEG_RAMP,     240u,      120000u }, { SIM_SEG_HOLD,   240u,  60000u },
    { SIM_SEG_RAMP,    1200u,      120000u }, { SIM_SEG_HOLD,  1200u,  60000u },
    { SIM_SEG_RAMP,    6000u,      120000u }, { SIM_SEG_HOLD,  6000u,  60000u },
    { SIM_SEG_RAMP,       0u,      120000u }, { SIM_SEG_HOLD,     0u, 120000u },
    /* tier 3 — 1 min windows: rates 10, 40, 100, -150                       */
    { SIM_SEG_RAMP,     600u,       60000u }, { SIM_SEG_HOLD,   600u,  30000u },
    { SIM_SEG_RAMP,    3000u,       60000u }, { SIM_SEG_HOLD,  3000u,  30000u },
    { SIM_SEG_RAMP,    9000u,       60000u }, { SIM_SEG_HOLD,  9000u,  30000u },
    { SIM_SEG_RAMP,       0u,       60000u }, { SIM_SEG_HOLD,     0u,  60000u },
    /* tier 4 — 30 s windows: rates 20, 80, 200, -300                        */
    { SIM_SEG_RAMP,     600u,       30000u }, { SIM_SEG_HOLD,   600u,  30000u },
    { SIM_SEG_RAMP,    3000u,       30000u }, { SIM_SEG_HOLD,  3000u,  30000u },
    { SIM_SEG_RAMP,    9000u,       30000u }, { SIM_SEG_HOLD,  9000u,  30000u },
    { SIM_SEG_RAMP,       0u,       30000u }, { SIM_SEG_HOLD,     0u,  60000u },
    /* tier 5 — 10 s windows: rates 50, 250, 700, -1000 (whole range in 10)  */
    { SIM_SEG_RAMP,     500u,       10000u }, { SIM_SEG_HOLD,   500u,  20000u },
    { SIM_SEG_RAMP,    3000u,       10000u }, { SIM_SEG_HOLD,  3000u,  20000u },
    { SIM_SEG_RAMP,   10000u,       10000u }, { SIM_SEG_HOLD, 10000u,  20000u },
    { SIM_SEG_RAMP,       0u,       10000u },
    /* closing stop — sized so the cycle lands on exactly 3,600,000 ms:
     * 35,000 status + 1,680,000 t1 + 780,000 t2 + 390,000 t3 + 270,000 t4
     * + 100,000 t5 = 3,255,000, leaving 345,000 here.                       */
    { SIM_SEG_HOLD,       0u,      345000u },
};

/* ---- Closing stop of a full run (1,000 s) -------------------------------- */
static const sim_seg_t sim_tab_stop[] =
{
    { SIM_SEG_HOLD,       0u,     1000000u },
};

#define SIM_N_A     (sizeof sim_tab_a    / sizeof sim_tab_a[0])
#define SIM_N_B     (sizeof sim_tab_b    / sizeof sim_tab_b[0])
#define SIM_N_STOP  (sizeof sim_tab_stop / sizeof sim_tab_stop[0])

/* A phase is a sequence of (table, repeat-count) parts. */
typedef struct
{
    const sim_seg_t *tab;
    uint32_t         n;
    uint32_t         reps;
} sim_part_t;

static const sim_part_t sim_parts_full[] =
{
    { sim_tab_a,    SIM_N_A,    1u                 },
    { sim_tab_b,    SIM_N_B,    SIM_PHASE_B_CYCLES },
    { sim_tab_stop, SIM_N_STOP, 1u                 },
};
static const sim_part_t sim_parts_a[] = { { sim_tab_a, SIM_N_A, 1u } };
static const sim_part_t sim_parts_b[] = { { sim_tab_b, SIM_N_B, 1u } };

static uint32_t sim_parts_of(uint8_t phase, const sim_part_t **out)
{
    if (phase == (uint8_t)SIM_PHASE_A) { *out = sim_parts_a;    return 1u; }
    if (phase == (uint8_t)SIM_PHASE_B) { *out = sim_parts_b;    return 1u; }
    *out = sim_parts_full;
    return (uint32_t)(sizeof sim_parts_full / sizeof sim_parts_full[0]);
}

/* Refresh count for a segment. Clamped to >= 1 so a pathologically slow RATE
 * can never produce a zero-length segment (which would divide by zero and
 * make the profile length wrong). At the supported RATE range (100-5000 ms)
 * the shortest segment (10 s) still yields 2 steps, so the clamp is defensive
 * only — but note it does lengthen the profile past spec if it ever fires.  */
static uint32_t sim_steps_of(uint32_t dur_ms, uint32_t rate_ms)
{
    uint32_t s = dur_ms / rate_ms;
    return (s == 0u) ? 1u : s;
}

/* Value at step k of a resolved segment. */
static uint16_t sim_value_at(const sim_seg_info_t *s, uint32_t k)
{
    if (s->kind == (uint8_t)SIM_SEG_STATUS)
    {
        /* Spread the seven status codes evenly, whatever the step count. */
        uint32_t i = (k * SIM_STATUS_CODES) / s->steps;
        if (i >= SIM_STATUS_CODES) { i = SIM_STATUS_CODES - 1u; }
        return (uint16_t)((uint32_t)LINK_CODE_NO_READING + i);
    }
    if (s->kind == (uint8_t)SIM_SEG_RAMP)
    {
        /* Integer linear interpolation. Using (k+1) means the LAST step lands
         * exactly on the target and the first step already moves, so the
         * previous segment's value is not repeated — that is what keeps the
         * Phase A sweep at exactly one code per refresh with no duplicates.
         *
         * 64-bit intermediate: at RATE 100 the Phase A ramp is 100,000 steps
         * with a 10,000 delta = 1e9, which still fits int32 — but the clamp
         * path above admits larger step counts, so the wide product is the
         * only bound-free choice. It runs once per refresh in the main loop
         * (never an ISR), so the M0 helper call is irrelevant.              */
        int32_t delta = (int32_t)s->target - (int32_t)s->start;
        int64_t num   = (int64_t)delta * (int64_t)(k + 1u);
        return (uint16_t)((int32_t)s->start + (int32_t)(num / (int64_t)s->steps));
    }
    return s->target;
}

/* Single walker behind every public query: iterates the phase's segments in
 * order, tracking each one's first index and entry value. Stops at the
 * segment matching either `index` (by_index) or `seg`.                      */
static bool sim_walk(uint8_t phase, uint32_t rate_ms, bool by_index,
                     uint32_t index, uint32_t seg_want,
                     sim_seg_info_t *out, uint32_t *seg_out)
{
    const sim_part_t *parts;
    uint32_t nparts = sim_parts_of(phase, &parts);
    uint32_t first  = 0u;
    uint16_t cur    = 0u;
    uint32_t seg    = 0u;
    uint32_t p, r, i;

    for (p = 0u; p < nparts; p++)
    {
        for (r = 0u; r < parts[p].reps; r++)
        {
            for (i = 0u; i < parts[p].n; i++)
            {
                const sim_seg_t *s     = &parts[p].tab[i];
                uint32_t         steps = sim_steps_of(s->dur_ms, rate_ms);
                bool             hit;

                /* Segments are visited in ascending index order, so an index
                 * below `first` wraps huge here and correctly misses.       */
                hit = by_index ? ((index - first) < steps) : (seg == seg_want);
                if (hit)
                {
                    out->kind   = s->kind;
                    out->start  = cur;
                    out->target = (s->kind == (uint8_t)SIM_SEG_STATUS)
                                      ? cur : s->target;
                    out->first  = first;
                    out->steps  = steps;
                    out->dur_ms = s->dur_ms;
                    if (seg_out != NULL) { *seg_out = seg; }
                    return true;
                }

                /* A status block reports codes, not pressure — it must not
                 * disturb the running value the next ramp starts from.      */
                if (s->kind != (uint8_t)SIM_SEG_STATUS) { cur = s->target; }
                first += steps;
                seg++;
            }
        }
    }
    return false;
}

uint32_t sim_profile_len(uint32_t rate_ms, uint8_t phase)
{
    const sim_part_t *parts;
    uint32_t nparts, p, r, i;
    uint32_t total = 0u;

    if (rate_ms == 0u) { rate_ms = 1u; }
    nparts = sim_parts_of(phase, &parts);
    for (p = 0u; p < nparts; p++)
    {
        for (r = 0u; r < parts[p].reps; r++)
        {
            for (i = 0u; i < parts[p].n; i++)
            {
                total += sim_steps_of(parts[p].tab[i].dur_ms, rate_ms);
            }
        }
    }
    return total;
}

uint32_t sim_profile_seg_count(uint8_t phase)
{
    const sim_part_t *parts;
    uint32_t nparts = sim_parts_of(phase, &parts);
    uint32_t p, total = 0u;

    for (p = 0u; p < nparts; p++)
    {
        total += parts[p].n * parts[p].reps;
    }
    return total;
}

bool sim_profile_seg_info(uint8_t phase, uint32_t seg, uint32_t rate_ms,
                          sim_seg_info_t *out)
{
    if ((out == NULL) || (rate_ms == 0u)) { return false; }
    return sim_walk(phase, rate_ms, false, 0u, seg, out, NULL);
}

uint32_t sim_profile_seg_at(uint32_t index, uint32_t rate_ms, uint8_t phase)
{
    sim_seg_info_t info;
    uint32_t       seg = 0u;
    uint32_t       len;

    if (rate_ms == 0u) { rate_ms = 1u; }
    len = sim_profile_len(rate_ms, phase);
    if (len == 0u) { return 0u; }
    index %= len;
    if (!sim_walk(phase, rate_ms, true, index, 0u, &info, &seg)) { return 0u; }
    return seg;
}

uint16_t sim_profile_code(uint32_t index, uint32_t rate_ms, uint8_t phase)
{
    sim_seg_info_t info;
    uint32_t       len;

    if (rate_ms == 0u) { rate_ms = 1u; }
    len = sim_profile_len(rate_ms, phase);
    if (len == 0u) { return (uint16_t)LINK_CODE_NO_READING; }
    index %= len;
    if (!sim_walk(phase, rate_ms, true, index, 0u, &info, NULL))
    {
        return (uint16_t)LINK_CODE_NO_READING;
    }
    return sim_value_at(&info, index - info.first);
}

#endif /* APP_ENABLE_SIM */
