#ifndef ACQUISITION_H
#define ACQUISITION_H

#include "types.h"

/*******************************************************************************
 * ADC1 acquisition — software-mode sampling of two probe channels.
 *
 * Each call to acquisition_run() samples both channels OVERSAMPLE_COUNT times
 * and divides the sum by OVERSAMPLE_DIV — production counts are 12-bit-SCALED
 * (0..ADC_COUNTS_MAX = 4092), keeping 2 bits of oversampling gain. `combined`
 * is the configured probe source (A, B, or the two-probe average — see
 * PROBE_MODE_*). The RAW/SCAN diagnostics stay in native 10-bit ADC units.
 ******************************************************************************/

typedef struct
{
    uint16 probe_a;         /* 12-bit-scaled average (0-4092), ADC_CH_PROBE_A */
    uint16 probe_b;         /* 12-bit-scaled average (0-4092), ADC_CH_PROBE_B */
    uint16 combined;        /* selected probe source (A / B / average)        */
    bool   stalled;         /* true: >= one channel's worth of conversions
                             * fell back (EOC timeout / invalid result) this
                             * run — readings are NOT trustworthy            */
} acq_result_t;

/* Per-channel raw statistics for one diagnostic burst (see acquisition_debug). */
typedef struct
{
    uint16 avg;             /* mean of OVERSAMPLE_COUNT raw conversions        */
    uint16 min;             /* smallest raw conversion in the burst            */
    uint16 max;             /* largest  raw conversion in the burst            */
    uint16 valid;           /* how many of the conversions reported valid (VF) */
    uint16 mv;              /* avg converted to millivolts (SDK attenuation)   */
} acq_chan_debug_t;

typedef struct
{
    acq_chan_debug_t a;
    acq_chan_debug_t b;
} acq_debug_t;

void acquisition_init(void);
void acquisition_run(acq_result_t *result);

/* Diagnostic: one fresh burst per channel, capturing raw spread + validity +
 * millivolts WITHOUT touching the production acquisition path. Use to tell a
 * sensor/amplifier problem (real but tiny signal) from an ADC/settling problem
 * (invalid reads or wild spread). */
void acquisition_debug(acq_debug_t *dbg);

/* Diagnostic: stats for one arbitrary ADC1 channel (used by the SCAN command to
 * sweep every external analog input and find where a live signal actually is). */
void acquisition_scan_channel(uint8 ch, acq_chan_debug_t *out);

/* Convert a P2.x ADC count to millivolts (SDK per-channel attenuation factor). */
uint16 acquisition_counts_to_mv(uint16 counts);

#endif /* ACQUISITION_H */
