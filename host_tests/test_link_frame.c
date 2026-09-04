/*******************************************************************************
 * Host test harness for app/link_frame.c — the ONLY executable verification
 * the link protocol gets before bench day.
 *
 * Part 1: pure-function vectors (checksum, encode, build).
 * Part 2: virtual-wire simulation — models real 9600 8N1 byte timing at
 *         microsecond resolution (TI at stop-bit START per the working
 *         hypothesis), a 1 ms tick, jittery main-loop service cadence, and
 *         deterministic stall/fence/busy scenarios. Asserts the wire-level
 *         protocol invariants the logger depends on.
 * Part 3: tick-wraparound test with scripted events.
 *
 * Emits golden_stream.csv (time_ms,byte) from the clean run for the Python
 * decoder tests in host_ui/.
 ******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "link_frame.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ==== Part 1: pure-function vectors ======================================= */

static void test_checksum(void)
{
    /* Independent expression of the spec: ~(msb+lsb) mod 256 */
    unsigned m, l;
    for (m = 0u; m < 256u; m += 17u)
    {
        for (l = 0u; l < 256u; l += 13u)
        {
            uint8_t want = (uint8_t)(~((m + l) & 0xFFu));
            CHECK(link_checksum((uint8_t)m, (uint8_t)l) == want,
                  "checksum(%02X,%02X)", m, l);
        }
    }
    /* Canonical vectors incl. 8-bit overflow */
    CHECK(link_checksum(0x27, 0x10) == 0xC8, "10000 vector");
    CHECK(link_checksum(0xFF, 0x01) == 0xFF, "NO_READING vector");
    CHECK(link_checksum(0x80, 0x90) == 0xEF, "overflow vector (0x110->0x10)");
    /* Transposition weakness is a DOCUMENTED property (additive sum):       */
    CHECK(link_checksum(0x12, 0x34) == link_checksum(0x34, 0x12),
          "additive checksum is transposition-blind (documented limitation)");
}

static void test_build_and_golden(void)
{
    /* Golden vectors — computed, never hand-trusted. Codes chosen to cover
     * zero, LSB rollover, 0x7F in LSB, 0x7F in checksum, full scale, and
     * every status code.                                                     */
    static const uint16_t codes[] = { 0u, 1u, 127u, 128u, 255u, 256u, 383u,
                                      10000u,
                                      0xFF01u, 0xFF02u, 0xFF03u, 0xFF04u,
                                      0xFF05u, 0xFF06u, 0xFF07u };
    size_t i;
    printf("golden vectors (code: MSB LSB CHK):\n");
    for (i = 0u; i < sizeof(codes) / sizeof(codes[0]); i++)
    {
        uint8_t d[3];
        uint16_t c = codes[i];
        link_build_data(c, d);
        CHECK(d[0] == (uint8_t)(c >> 8),   "MSB of %u", c);
        CHECK(d[1] == (uint8_t)(c & 0xFF), "LSB of %u", c);
        CHECK(d[2] == (uint8_t)(~((d[0] + d[1]) & 0xFF)), "CHK of %u", c);
        printf("  %5u (0x%04X): %02X %02X %02X\n", c, c, d[0], d[1], d[2]);
    }
    /* PAYLOAD-SACRED pass-through: 0x7F must appear verbatim.               */
    {
        uint8_t d[3];
        link_build_data(127u, d);   /* LSB == 0x7F */
        CHECK(d[0] == 0x00 && d[1] == 0x7F && d[2] == 0x80,
              "code 127 must emit 00 7F 80 unmodified");
        link_build_data(383u, d);   /* LSB == 0x7F AND checksum == 0x7F */
        CHECK(d[0] == 0x01 && d[1] == 0x7F && d[2] == 0x7F,
              "code 383 must emit 01 7F 7F unmodified");
    }
    /* Bijectivity over the whole pressure range.                             */
    {
        uint32_t c;
        for (c = 0u; c <= 10000u; c++)
        {
            uint8_t d[3];
            link_build_data((uint16_t)c, d);
            CHECK((uint16_t)((d[0] << 8) | d[1]) == c, "bijective %u", c);
        }
    }
}

static void test_encode(void)
{
    /* Exact, unambiguous cases */
    CHECK(link_encode_bar(0.0f)     == 0u,     "0.0");
    CHECK(link_encode_bar(-0.0f)    == 0u,     "-0.0");
    CHECK(link_encode_bar(-4.9f)    == 0u,     "-4.9 clamps to 0");
    CHECK(link_encode_bar(-5.0f)    == 0u,     "-5.0 exactly clamps to 0");
    CHECK(link_encode_bar(1000.0f)  == 10000u, "full scale");
    CHECK(link_encode_bar(1009.9f)  == 10000u, "tolerance clamp POLICY");
    CHECK(link_encode_bar(1010.0f)  == 10000u, "1010.0 exactly clamps");
    CHECK(link_encode_bar(500.0f)   == 5000u,  "mid scale");
    CHECK(link_encode_bar(nextafterf(-5.0f, -INFINITY)) == LINK_CODE_UNDER_RANGE,
          "just below -5.0 -> UNDER_RANGE");
    CHECK(link_encode_bar(-5.1f)    == LINK_CODE_UNDER_RANGE, "-5.1");
    CHECK(link_encode_bar(nextafterf(1010.0f, INFINITY)) == LINK_CODE_OVER_RANGE,
          "just above 1010.0 -> OVER_RANGE");
    CHECK(link_encode_bar(1010.1f)  == LINK_CODE_OVER_RANGE, "1010.1");
    /* Non-finite must short-circuit to UNCAL (UB otherwise)                 */
    CHECK(link_encode_bar(NAN)       == LINK_CODE_UNCAL, "NaN");
    CHECK(link_encode_bar(INFINITY)  == LINK_CODE_UNCAL, "+Inf");
    CHECK(link_encode_bar(-INFINITY) == LINK_CODE_UNCAL, "-Inf");

    /* Round-half-up correctness within float limits: for every in-range
     * result, |code - 10*bar| <= 0.5 (+ float slack), and encode is
     * monotone non-decreasing across boundary neighbors.                    */
    {
        static const float bounds[] = { 0.05f, 0.1f, 12.7f, 12.75f, 38.35f,
                                        63.85f, 500.05f, 999.9f, 999.94f,
                                        999.95f, 1000.0f };
        size_t i;
        for (i = 0u; i < sizeof(bounds) / sizeof(bounds[0]); i++)
        {
            float b  = bounds[i];
            float lo = nextafterf(b, -INFINITY);
            float hi = nextafterf(b, INFINITY);
            uint16_t cl = link_encode_bar(lo);
            uint16_t cb = link_encode_bar(b);
            uint16_t ch = link_encode_bar(hi);
            CHECK(cl <= cb && cb <= ch, "monotone at %.6f", (double)b);
            CHECK(((int)ch - (int)cl) <= 1, "neighbor jump <=1 LSB at %.6f", (double)b);
            CHECK(fabsf((float)cb - (b * 10.0f)) <= 0.5001f,
                  "half-up within 0.5 LSB at %.6f (got %u)", (double)b, cb);
        }
    }
    /* Round-trip: every wire code decodes to a bar that re-encodes to
     * itself (bijectivity through the float path).                          */
    {
        uint32_t c;
        for (c = 0u; c <= 10000u; c++)
        {
            float bar = (float)c / 10.0f;
            CHECK(link_encode_bar(bar) == c, "round-trip %u", c);
        }
    }
    /* Result never falls in the status page unless it IS a status.          */
    CHECK(link_encode_bar(999.99f) < 0xFF00u, "no accidental status code");
}

/* ==== Part 2: virtual-wire simulation ===================================== */

#define BIT_US   104.1667
#define BYTE_US  (10.0 * BIT_US)
#define TI_US    (9.0 * BIT_US)     /* working hypothesis: TI at stop start  */

typedef struct { double start_us; double end_us; uint8_t byte; } wbyte_t;

typedef struct
{
    double   t_us;              /* simulation clock                          */
    wbyte_t  wire[4096];
    int      wire_n;
    double   wire_busy_until;   /* physical line busy until                  */
    double   ti_pending_at;     /* TI of last byte of queued transmission    */
    int      ti_pending;        /* 1 = tx_done not yet delivered to SM       */
    uint32_t lcg;               /* deterministic jitter                      */
    link_sm_t sm;
    uint16_t live_code;
    int      held;
    int      packet_mode;
    int      drop_next_ti;      /* fault injection: swallow the next TI      */
} sim_t;

static double lcg_jitter_us(sim_t *s, double max_us)
{
    s->lcg = s->lcg * 1664525u + 1013904223u;
    return ((double)(s->lcg >> 8) / 16777216.0) * max_us;
}

static uint32_t sim_tick(const sim_t *s) { return (uint32_t)(s->t_us / 1000.0); }

static void sim_queue_bytes(sim_t *s, const uint8_t *b, int n)
{
    int i;
    double start = s->t_us;
    if (start < s->wire_busy_until) { start = s->wire_busy_until; }
    for (i = 0; i < n; i++)
    {
        if (s->wire_n < (int)(sizeof(s->wire) / sizeof(s->wire[0])))
        {
            s->wire[s->wire_n].start_us = start + (double)i * BYTE_US;
            s->wire[s->wire_n].end_us   = start + (double)(i + 1) * BYTE_US;
            s->wire[s->wire_n].byte     = b[i];
            s->wire_n++;
        }
    }
    s->wire_busy_until = start + (double)n * BYTE_US;
    s->ti_pending_at   = start + (double)(n - 1) * BYTE_US + TI_US;
    if (s->drop_next_ti) { s->drop_next_ti = 0; s->ti_pending = 0; }
    else                 { s->ti_pending = 1; }
}

static void sim_service(sim_t *s)
{
    link_sm_in_t in;
    link_action_t act;
    memset(&in, 0, sizeof(in));
    in.now         = sim_tick(s);
    in.live_code   = s->live_code;
    in.uart_idle   = (s->t_us >= s->wire_busy_until);
    in.held        = (s->held != 0);
    in.packet_mode = (s->packet_mode != 0);
    if (s->ti_pending && (s->t_us >= s->ti_pending_at))
    {
        in.tx_done      = true;
        in.tx_done_tick = (uint32_t)(s->ti_pending_at / 1000.0);
        s->ti_pending   = 0;
    }
    act = link_sm_step(&s->sm, &in);
    if (act == LINK_ACT_SEND_SYNC)
    {
        uint8_t sb = LINK_SYNC_BYTE;
        sim_queue_bytes(s, &sb, 1);
    }
    else if (act == LINK_ACT_SEND_DATA)
    {
        sim_queue_bytes(s, s->sm.data, 3);
    }
}

static void sim_init(sim_t *s, double t0_us)
{
    memset(s, 0, sizeof(*s));
    s->t_us = t0_us;
    s->wire_busy_until = t0_us;
    s->lcg  = 0xC0FFEEu;
    s->live_code = LINK_CODE_NO_READING;
    s->packet_mode = 1;
    link_sm_init(&s->sm, sim_tick(s));
}

/* Run with service cadence base_dt + jitter; optional freeze window during
 * which the SM is NOT serviced (time still advances — a blocked main loop). */
static void sim_run(sim_t *s, double dur_us, double freeze_at_us,
                    double freeze_len_us)
{
    double t_end = s->t_us + dur_us;
    double frozen_until = -1.0;
    if (freeze_len_us > 0.0) { frozen_until = freeze_at_us + freeze_len_us; }
    while (s->t_us < t_end)
    {
        if (freeze_len_us > 0.0 &&
            s->t_us >= freeze_at_us && s->t_us < frozen_until)
        {
            s->t_us = frozen_until;   /* main loop blocked: no service       */
            continue;
        }
        sim_service(s);
        s->t_us += 200.0 + lcg_jitter_us(s, 800.0);  /* 0.2..1.0 ms cadence  */
    }
}

/* Wire-level packet validation. Returns number of complete packets found.   */
typedef struct { int packets; int orphan_syncs; double min_gap, max_gap;
                 double max_total, min_idle, min_period; } wire_stats_t;

static void validate_wire(const sim_t *s, wire_stats_t *st,
                          double ignore_before_us)
{
    int i = 0;
    double prev_pkt_end = -1.0, prev_sync_start = -1.0;
    st->packets = 0; st->orphan_syncs = 0;
    st->min_gap = 1e9; st->max_gap = 0.0; st->max_total = 0.0;
    st->min_idle = 1e9; st->min_period = 1e9;
    while (i < s->wire_n)
    {
        if (s->wire[i].byte == LINK_SYNC_BYTE && s->wire[i].start_us >= ignore_before_us)
        {
            double sync_start = s->wire[i].start_us;
            double sync_end   = s->wire[i].end_us;
            /* Capture may end mid-packet: a trailing sync whose data block
             * simply hasn't been transmitted yet is NOT an abort fragment.  */
            if ((i + 1) >= s->wire_n && (s->t_us - sync_start) < 10000.0)
            {
                break;
            }
            /* A data block follows within the packet window, or this was an
             * aborted (sync-only) packet.                                    */
            if ((i + 3) < s->wire_n &&
                (s->wire[i + 1].start_us - sync_end) < 8000.0)
            {
                double gap   = s->wire[i + 1].start_us - sync_end;
                double total = s->wire[i + 3].end_us - sync_start;
                uint8_t chk  = (uint8_t)(~((s->wire[i + 1].byte +
                                            s->wire[i + 2].byte) & 0xFF));
                CHECK(s->wire[i + 3].byte == chk, "wire checksum @%d", i);
                if (gap < st->min_gap)     { st->min_gap = gap; }
                if (gap > st->max_gap)     { st->max_gap = gap; }
                if (total > st->max_total) { st->max_total = total; }
                if (prev_pkt_end > 0.0)
                {
                    double idle = sync_start - prev_pkt_end;
                    if (idle < st->min_idle) { st->min_idle = idle; }
                }
                if (prev_sync_start > 0.0)
                {
                    double per = sync_start - prev_sync_start;
                    if (per < st->min_period) { st->min_period = per; }
                }
                prev_pkt_end    = s->wire[i + 3].end_us;
                prev_sync_start = sync_start;
                st->packets++;
                i += 4;
                continue;
            }
            st->orphan_syncs++;
            /* An orphan sync still occupies the wire — idle for the NEXT
             * packet is measured from its end.                              */
            prev_pkt_end = sync_end;
        }
        i++;
    }
}

static void test_sim_clean(void)
{
    sim_t *s = malloc(sizeof(sim_t));
    wire_stats_t st;
    FILE *f;
    int i;
    sim_init(s, 0.0);
    s->live_code = 1234u;    /* 123.4 bar */
    sim_run(s, 10.0e6, 0.0, 0.0);   /* 10 s, no faults */
    validate_wire(s, &st, 0.0);
    printf("clean run: %d packets, gap [%.2f, %.2f] ms, total max %.2f ms, "
           "idle min %.2f ms, period min %.2f ms\n",
           st.packets, st.min_gap / 1000.0, st.max_gap / 1000.0,
           st.max_total / 1000.0, st.min_idle / 1000.0,
           st.min_period / 1000.0);
    CHECK(st.packets >= (int)(10000u / LINK_PACKET_PERIOD_MS) - 5 &&
          st.packets <= (int)(10000u / LINK_PACKET_PERIOD_MS) + 1,
          "≈%u pkts in 10 s (%d)", 10000u / LINK_PACKET_PERIOD_MS, st.packets);
    CHECK(st.orphan_syncs == 0, "no orphan syncs in clean run");
    CHECK(st.min_gap  >= 2000.0, "gap >2 ms (worst %.0f us)", st.min_gap);
    CHECK(st.max_gap  <= 5000.0, "gap <=5 ms (worst %.0f us)", st.max_gap);
    CHECK(st.max_total <= 9300.0, "packet <=9.3 ms (worst %.0f us)", st.max_total);
    CHECK(st.min_idle >= 20000.0, "idle >20 ms (worst %.0f us)", st.min_idle);
    CHECK(st.min_period >= (LINK_PACKET_PERIOD_MS * 1000.0) - 1100.0,
          "period ≈%u ms (worst %.0f us)", LINK_PACKET_PERIOD_MS, st.min_period);
    for (i = 0; i < (int)LINK_ABT_COUNT; i++)
    {
        CHECK(s->sm.aborts[i] == 0, "zero aborts (origin %d)", i);
    }
    CHECK(s->sm.busy_skips == 0, "zero busy skips");
    /* First packet promptness: stream alive from boot.                      */
    CHECK(s->wire_n > 0 && s->wire[0].start_us < 5000.0,
          "first sync within 5 ms of boot");
    /* Latched code on the wire matches live_code image.                     */
    CHECK(s->wire[1].byte == (1234u >> 8) && s->wire[2].byte == (1234u & 0xFF),
          "wire carries latched code");
    /* Emit golden stream for the Python decoder tests.                      */
    f = fopen("golden_stream.csv", "w");
    if (f != NULL)
    {
        fprintf(f, "time_ms,byte\n");
        for (i = 0; i < s->wire_n; i++)
        {
            fprintf(f, "%.3f,%u\n", s->wire[i].start_us / 1000.0,
                    s->wire[i].byte);
        }
        fclose(f);
        printf("golden_stream.csv: %d bytes emitted\n", s->wire_n);
    }
    else { CHECK(0, "could not write golden_stream.csv"); }
    free(s);
}

static void test_sim_gap_stall(void)
{
    /* Freeze the main loop for 15 ms starting inside the GAP phase of the
     * 2nd packet -> exactly one DATA_DEADLINE abort, sync-only fragment on
     * the wire, full recovery, clean packets after.                          */
    sim_t *s = malloc(sizeof(sim_t));
    wire_stats_t st;
    sim_init(s, 0.0);
    s->live_code = 5000u;
    /* Packet 2 sync starts ≈PERIOD; its GAP phase spans the ~3 ms after.   */
    sim_run(s, 2.0e6, (LINK_PACKET_PERIOD_MS + 2u) * 1000.0, 15000.0);
    validate_wire(s, &st, 0.0);
    CHECK(s->sm.aborts[LINK_ABT_DATA_DEADLINE] == 1,
          "exactly one deadline abort (got %u)",
          s->sm.aborts[LINK_ABT_DATA_DEADLINE]);
    CHECK(st.orphan_syncs == 1, "one sync-only fragment (got %d)", st.orphan_syncs);
    CHECK(st.max_total <= 9300.0, "no packet exceeds deadline even around stall");
    CHECK(st.min_idle >= 20000.0, "idle preserved through recovery (worst %.0f us)",
          st.min_idle);
    CHECK(st.packets >= (int)(2000u / LINK_PACKET_PERIOD_MS) - 2,
          "stream recovers (%d pkts in 2 s)", st.packets);
    free(s);
}

static void test_sim_fence_hold(void)
{
    /* 34 ms 'stalled acquisition' guarded by the fence: hold while IDLE,
     * freeze, release -> NO abort, no fragment, packet postponed once, no
     * catch-up burst.                                                        */
    sim_t *s = malloc(sizeof(sim_t));
    wire_stats_t st;
    int i;
    sim_init(s, 0.0);
    s->live_code = 7777u;
    sim_run(s, 60000.0, 0.0, 0.0);           /* past packet 1 (ends ~9ms)    */
    CHECK(s->sm.st == LINK_ST_IDLE, "IDLE before fence");
    s->held = 1;                              /* fence acquired               */
    sim_run(s, 34000.0, s->t_us, 34000.0);    /* frozen main loop 34 ms       */
    s->held = 0;                              /* release: transmits NOTHING   */
    CHECK(s->sm.st == LINK_ST_IDLE, "release itself sends nothing");
    sim_run(s, 2.0e6, 0.0, 0.0);
    validate_wire(s, &st, 0.0);
    for (i = 0; i < (int)LINK_ABT_COUNT; i++)
    {
        CHECK(s->sm.aborts[i] == 0, "fence-held stall causes no abort (%d)", i);
    }
    CHECK(st.orphan_syncs == 0, "no fragments with fence");
    CHECK(st.min_idle >= 20000.0, "idle preserved across hold");
    /* Max gap between packets bounded by period + hold + service slack.     */
    {
        double max_gap_between = 0.0, prev = -1.0;
        for (i = 0; i + 3 < s->wire_n; i++)
        {
            if (s->wire[i].byte == LINK_SYNC_BYTE)
            {
                if (prev > 0.0 && (s->wire[i].start_us - prev) > max_gap_between)
                {
                    max_gap_between = s->wire[i].start_us - prev;
                }
                prev = s->wire[i].start_us;
                i += 3;
            }
        }
        CHECK(max_gap_between <= (LINK_PACKET_PERIOD_MS * 1000.0) + 38000.0,
              "max sync-to-sync <=%.0f ms under 34 ms hold (got %.1f ms)",
              LINK_PACKET_PERIOD_MS + 38.0, max_gap_between / 1000.0);
    }
    free(s);
}

static void test_sim_sync_ti_lost(void)
{
    /* UART swallows the sync byte's TI -> SYNC_TIMEOUT abort, conservative
     * recovery, stream resumes cleanly.                                      */
    sim_t *s = malloc(sizeof(sim_t));
    wire_stats_t st;
    sim_init(s, 0.0);
    s->live_code = 42u;
    sim_run(s, 30000.0, 0.0, 0.0);      /* packet 1 completes                */
    s->drop_next_ti = 1;                 /* packet 2's sync TI vanishes       */
    sim_run(s, 2.0e6, 0.0, 0.0);
    validate_wire(s, &st, 0.0);
    CHECK(s->sm.aborts[LINK_ABT_SYNC_TIMEOUT] == 1, "one sync-timeout abort");
    CHECK(st.min_idle >= 20000.0, "idle preserved after TI loss");
    CHECK(st.packets >= (int)(2000u / LINK_PACKET_PERIOD_MS) - 2,
          "stream recovers after TI loss (%d)", st.packets);
    free(s);
}

static void test_sim_modes_and_latch(void)
{
    sim_t *s = malloc(sizeof(sim_t));
    int n_before;
    sim_init(s, 0.0);
    /* Bench console mode: no packets at all.                                */
    s->packet_mode = 0;
    sim_run(s, 500000.0, 0.0, 0.0);
    CHECK(s->wire_n == 0, "console mode: zero packet bytes (got %d)", s->wire_n);
    /* Back to packet mode: resumes.                                         */
    s->packet_mode = 1;
    sim_run(s, 100000.0, 0.0, 0.0);
    CHECK(s->wire_n >= 4, "packet mode resumes");
    /* Latch: change live_code right after sync goes out; wire must carry
     * the value latched at sync start.                                      */
    n_before = s->wire_n;
    s->live_code = 1111u;
    sim_run(s, 41000.0, 0.0, 0.0);        /* next packet latches 1111        */
    s->live_code = 2222u;                  /* changed AFTER latch             */
    sim_run(s, 20000.0, 0.0, 0.0);         /* its data block goes out         */
    CHECK(s->wire_n >= n_before + 4, "a packet completed");
    {
        int i, found = 0;
        for (i = n_before; i + 3 < s->wire_n; i++)
        {
            if (s->wire[i].byte == LINK_SYNC_BYTE)
            {
                uint16_t v = (uint16_t)((s->wire[i + 1].byte << 8) |
                                        s->wire[i + 2].byte);
                CHECK(v == 1111u, "latched code immutable mid-packet (got %u)", v);
                found = 1;
                break;
            }
        }
        CHECK(found, "latch-test packet found");
    }
    free(s);
}

static void test_busy_skip_scripted(void)
{
    /* Deterministic: SM in IDLE, packet due, UART not idle -> counted skip,
     * no state change, no abort; retried and fired once the line frees.     */
    link_sm_t sm;
    link_sm_in_t in;
    link_action_t act;
    link_sm_init(&sm, 1000u);
    memset(&in, 0, sizeof(in));
    in.packet_mode = true;
    in.now = 1000u; in.uart_idle = false;      /* due (pre-aged) but busy    */
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_NONE && sm.st == LINK_ST_IDLE, "busy: no sync");
    CHECK(sm.busy_skips == 1u, "busy skip counted (%u)", sm.busy_skips);
    in.now = 1001u;
    act = link_sm_step(&sm, &in);
    CHECK(sm.busy_skips == 2u, "busy skip counted again");
    in.uart_idle = true;
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_SEND_SYNC, "fires once line frees");
    {
        int i, total_aborts = 0;
        for (i = 0; i < (int)LINK_ABT_COUNT; i++) { total_aborts += sm.aborts[i]; }
        CHECK(total_aborts == 0, "busy != abort");
    }
}

/* ==== Part 3: tick wraparound (scripted, no wire model) =================== */

static void test_wraparound(void)
{
    link_sm_t sm;
    link_sm_in_t in;
    uint32_t t0 = 0xFFFFFFF0u;   /* wraps 16 ticks in                        */
    link_action_t act;
    link_sm_init(&sm, t0);
    memset(&in, 0, sizeof(in));
    in.uart_idle = true; in.packet_mode = true;
    in.now = t0;
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_SEND_SYNC, "first sync fires at pre-wrap tick");
    in.now = t0 + 1u; in.tx_done = true; in.tx_done_tick = t0 + 1u;
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_NONE && sm.st == LINK_ST_GAP, "SYNC->GAP pre-wrap");
    in.tx_done = false;
    in.now = t0 + 5u;                       /* 0xFFFFFFF5: gap elapsed        */
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_SEND_DATA, "GAP->DATA across nothing yet");
    in.now = t0 + 9u; in.tx_done = true; in.tx_done_tick = t0 + 9u;
    act = link_sm_step(&sm, &in);
    CHECK(sm.st == LINK_ST_IDLE && sm.pkts_ok == 1u, "packet 1 done pre-wrap");
    in.tx_done = false;
    /* Next packet due one PERIOD out — PAST the wrap.                       */
    in.now = t0 + LINK_PACKET_PERIOD_MS - 10u;
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_NONE, "not due yet across wrap");
    in.now = t0 + LINK_PACKET_PERIOD_MS;
    act = link_sm_step(&sm, &in);
    CHECK(act == LINK_ACT_SEND_SYNC, "period fires correctly across wrap");
    CHECK(sm.aborts[LINK_ABT_BAD_STATE] == 0, "no wrap corruption");
}


/* ==== Part 4: bench simulation profile ====================================
 * The profile is the reference stream for the 24 h logging soak, so these
 * tests ARE the contract: the host verifier compares captures against the
 * very same sim_profile_code() compiled here.
 * ========================================================================= */

/* dbar/refresh for each of the 20 Phase B ramps at RATE 1000, in table order.
 * Written out independently of link_frame.c so a table edit has to be a
 * deliberate two-place change, not a silent one.                           */
static const int32_t k_ladder_rates[20] = {
    1,   5,   14,  -20,      /* tier 1 — 5 min windows  */
    2,   8,   40,  -50,      /* tier 2 — 2 min windows  */
    10,  40,  100, -150,     /* tier 3 — 1 min windows  */
    20,  80,  200, -300,     /* tier 4 — 30 s windows   */
    50,  250, 700, -1000     /* tier 5 — 10 s windows   */
};

static void test_sim_lengths(void)
{
    /* Durations are wall-clock, so refresh counts scale with RATE. */
    CHECK(sim_profile_len(1000u, SIM_PHASE_A) == 20600u,
          "phase A @1000 = %u, want 20600", sim_profile_len(1000u, SIM_PHASE_A));
    CHECK(sim_profile_len(1000u, SIM_PHASE_B) == 3600u,
          "phase B @1000 = %u, want 3600", sim_profile_len(1000u, SIM_PHASE_B));
    CHECK(sim_profile_len(1000u, SIM_PHASE_FULL) == 86400u,
          "FULL @1000 = %u, want 86400 (24 h)",
          sim_profile_len(1000u, SIM_PHASE_FULL));

    CHECK(sim_profile_len(100u, SIM_PHASE_A) == 206000u, "phase A @100");
    CHECK(sim_profile_len(100u, SIM_PHASE_B) == 36000u,  "phase B @100");
    CHECK(sim_profile_len(100u, SIM_PHASE_FULL) == 864000u, "FULL @100");

    /* FULL must be exactly A + 18*B + the closing stop (1000 s). */
    CHECK(sim_profile_len(1000u, SIM_PHASE_FULL)
              == 20600u + 18u * 3600u + 1000u, "FULL composition");
}

/* Every valid code, once ascending and once descending — the resolution and
 * full-range proof, and what covers the payload-sacred 0x7F images without a
 * hand-written vector list.
 *
 * The no-duplicate contract belongs to the RAMP segments specifically: the
 * holds either side legitimately repeat 0 and full scale, so a naive
 * first-half/second-half split would (wrongly) score those as duplicates. */
static void test_sim_phase_a_coverage(uint32_t rate)
{
    static uint8_t seen[10001];
    uint32_t nseg = sim_profile_seg_count(SIM_PHASE_A);
    uint32_t seg, i, ramps = 0u;
    int miss_all = 0, bad = 0;
    int seen_7f_lsb = 0, seen_7f_chk = 0;
    uint32_t len = sim_profile_len(rate, SIM_PHASE_A);

    memset(seen, 0, sizeof seen);

    /* Whole-phase pass: total coverage and code-page legality. */
    for (i = 0u; i < len; i++)
    {
        uint16_t c = sim_profile_code(i, rate, SIM_PHASE_A);
        uint8_t  d[3];
        if (c > LINK_VALUE_MAX)
        {
            if (c < LINK_CODE_NO_READING || c > LINK_CODE_UNDER_RANGE) { bad++; }
            continue;
        }
        seen[c] = 1u;
        link_build_data(c, d);
        if (d[1] == LINK_SYNC_BYTE) { seen_7f_lsb++; }
        if (d[2] == LINK_SYNC_BYTE) { seen_7f_chk++; }
    }
    for (i = 0u; i <= LINK_VALUE_MAX; i++) { if (!seen[i]) { miss_all++; } }

    CHECK(bad == 0, "phase A @%u emitted %d out-of-page codes", rate, bad);
    CHECK(miss_all == 0, "phase A @%u missed %d of the 10001 codes",
          rate, miss_all);
    CHECK(seen_7f_lsb > 0, "phase A never produced an LSB of 0x7F");
    CHECK(seen_7f_chk > 0, "phase A never produced a checksum of 0x7F");

    /* Per-ramp pass: each sweep must be strictly monotonic and, at the
     * one-code-per-refresh rate, hit each code exactly once. */
    for (seg = 0u; seg < nseg; seg++)
    {
        sim_seg_info_t sg;
        uint32_t k;
        int dups = 0;
        uint16_t prev;
        if (!sim_profile_seg_info(SIM_PHASE_A, seg, rate, &sg)) { continue; }
        if (sg.kind != SIM_SEG_RAMP) { continue; }
        ramps++;
        memset(seen, 0, sizeof seen);
        prev = sg.start;
        for (k = 0u; k < sg.steps; k++)
        {
            uint16_t c = sim_profile_code(sg.first + k, rate, SIM_PHASE_A);
            CHECK(c <= LINK_VALUE_MAX, "phase A ramp exceeded the cap: %u", c);
            if (sg.target > sg.start) { CHECK(c >= prev, "phase A ramp up not monotonic"); }
            else                      { CHECK(c <= prev, "phase A ramp down not monotonic"); }
            if (seen[c]) { dups++; }
            seen[c] = 1u;
            prev = c;
        }
        CHECK(prev == sg.target, "phase A ramp ended at %u, target %u",
              prev, sg.target);
        if (rate == 1000u)
        {
            /* Exactly one code per refresh: 10000 steps, 10000 distinct codes. */
            CHECK(dups == 0, "phase A ramp @1000 repeated %d codes", dups);
            CHECK(sg.steps == 10000u, "phase A ramp @1000 has %u steps, want 10000",
                  sg.steps);
        }
    }
    CHECK(ramps == 2u, "phase A has %u ramps, expected 2 (up and down)", ramps);
}

/* Fixed-duration windows at varying rates — the timing-ladder contract. */
static void test_sim_ladder(void)
{
    const uint32_t rate = 1000u;
    uint32_t nseg = sim_profile_seg_count(SIM_PHASE_B);
    uint32_t seg, ramp_i = 0u, holds = 0u, status = 0u, walked = 0u;

    for (seg = 0u; seg < nseg; seg++)
    {
        sim_seg_info_t s;
        uint32_t k;
        if (!sim_profile_seg_info(SIM_PHASE_B, seg, rate, &s))
        {
            CHECK(0, "seg_info(%u) failed", seg);
            return;
        }
        CHECK(s.first == walked, "seg %u starts at %u, expected %u",
              seg, s.first, walked);
        CHECK(s.steps == s.dur_ms / rate, "seg %u step count", seg);
        walked += s.steps;

        if (s.kind == SIM_SEG_STATUS)
        {
            status++;
            /* Exactly the seven codes, in order, evenly spread. */
            for (k = 0u; k < s.steps; k++)
            {
                uint16_t c = sim_profile_code(s.first + k, rate, SIM_PHASE_B);
                uint16_t want = (uint16_t)(LINK_CODE_NO_READING
                                           + (k * SIM_STATUS_CODES) / s.steps);
                CHECK(c == want, "status seg step %u: got %04X want %04X",
                      k, c, want);
            }
        }
        else if (s.kind == SIM_SEG_HOLD)
        {
            holds++;
            for (k = 0u; k < s.steps; k++)
            {
                uint16_t c = sim_profile_code(s.first + k, rate, SIM_PHASE_B);
                CHECK(c == s.target, "hold seg %u step %u drifted: %u != %u",
                      seg, k, c, s.target);
            }
        }
        else /* RAMP */
        {
            int32_t  delta = (int32_t)s.target - (int32_t)s.start;
            int32_t  rate_dbar;
            int      up = (delta > 0);
            uint16_t prev = s.start;

            CHECK(ramp_i < 20u, "more than 20 ramps in the ladder");
            /* Rates must be whole dbar/refresh at RATE 1000 — that is what
             * makes the ladder's expected values trivially checkable.      */
            CHECK((delta % (int32_t)s.steps) == 0,
                  "ramp %u rate not integral at RATE 1000", ramp_i);
            rate_dbar = delta / (int32_t)s.steps;
            if (ramp_i < 20u)
            {
                CHECK(rate_dbar == k_ladder_rates[ramp_i],
                      "ramp %u rate %d dbar/rf, expected %d",
                      ramp_i, rate_dbar, k_ladder_rates[ramp_i]);
            }
            /* Monotonic, no overshoot, exact landing on the target. */
            for (k = 0u; k < s.steps; k++)
            {
                uint16_t c = sim_profile_code(s.first + k, rate, SIM_PHASE_B);
                CHECK(c <= LINK_VALUE_MAX, "ramp %u exceeded the cap: %u",
                      ramp_i, c);
                if (up) { CHECK(c >= prev, "ramp %u not monotonic up", ramp_i); }
                else    { CHECK(c <= prev, "ramp %u not monotonic down", ramp_i); }
                prev = c;
            }
            CHECK(prev == s.target, "ramp %u ended at %u, target %u",
                  ramp_i, prev, s.target);
            ramp_i++;
        }
    }
    CHECK(ramp_i == 20u, "ladder has %u ramps, expected 20", ramp_i);
    CHECK(status == 1u, "ladder has %u status blocks, expected 1", status);
    CHECK(holds > 0u, "ladder has no holds");
    CHECK(walked == sim_profile_len(rate, SIM_PHASE_B),
          "segment walk (%u) != profile length (%u)",
          walked, sim_profile_len(rate, SIM_PHASE_B));
}

/* Pure and wrap-seamless: the soak reference must be reproducible and the
 * index must be free to run past the end of the profile. */
static void test_sim_determinism_and_wrap(void)
{
    uint32_t len = sim_profile_len(1000u, SIM_PHASE_FULL);
    uint32_t i;
    for (i = 0u; i < 5000u; i++)
    {
        uint32_t idx = i * 17u;
        uint16_t a = sim_profile_code(idx, 1000u, SIM_PHASE_FULL);
        uint16_t b = sim_profile_code(idx, 1000u, SIM_PHASE_FULL);
        uint16_t w = sim_profile_code(idx + len, 1000u, SIM_PHASE_FULL);
        CHECK(a == b, "sim_profile_code not pure at %u", idx);
        CHECK(a == w, "wrap discontinuity at %u", idx);
    }
    /* A degenerate RATE must not divide by zero or produce a zero length. */
    CHECK(sim_profile_len(0u, SIM_PHASE_A) > 0u, "rate 0 guarded");
    CHECK(sim_profile_len(5000u, SIM_PHASE_B) > 0u, "slow rate still valid");
}

/* Emit the reference stream the host verifier scores captures against. */
static int emit_reference(const char *path, uint32_t rate, uint8_t phase)
{
    FILE *f = fopen(path, "w");
    uint32_t len, i;
    if (f == NULL) { printf("cannot write %s\n", path); return 1; }
    len = sim_profile_len(rate, phase);
    fprintf(f, "index,code,seg\n");
    for (i = 0u; i < len; i++)
    {
        fprintf(f, "%u,%u,%u\n", i, sim_profile_code(i, rate, phase),
                sim_profile_seg_at(i, rate, phase));
    }
    fclose(f);
    printf("%s: %u rows (rate=%u ms, phase=%u)\n", path, len, rate, phase);
    return 0;
}

int main(int argc, char **argv)
{
    /* Reference-stream generation for host_ui/soak_verify.py:
     *   ./test_link_frame --emit-ref <file> <rate_ms> <phase 0=FULL,1=A,2=B>
     * Generated from the SAME function the firmware runs, which is the whole
     * reason the profile lives in the pure protocol core.                  */
    if (argc >= 5 && strcmp(argv[1], "--emit-ref") == 0)
    {
        return emit_reference(argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                              (uint8_t)strtoul(argv[4], NULL, 10));
    }
    (void)argc; (void)argv;
    test_checksum();
    test_build_and_golden();
    test_encode();
    test_sim_clean();
    test_sim_gap_stall();
    test_sim_fence_hold();
    test_sim_sync_ti_lost();
    test_sim_modes_and_latch();
    test_busy_skip_scripted();
    test_wraparound();
    test_sim_lengths();
    test_sim_phase_a_coverage(1000u);
    test_sim_phase_a_coverage(100u);
    test_sim_ladder();
    test_sim_determinism_and_wrap();
    if (g_fail != 0)
    {
        printf("\n%d FAILURE(S)\n", g_fail);
        return 1;
    }
    printf("\nALL TESTS PASSED\n");
    return 0;
}
