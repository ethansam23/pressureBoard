#include "link_frame.h"
#include <math.h>   /* isfinite, floorf — softfloat on M0, main-loop only    */

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
