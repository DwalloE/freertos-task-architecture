/*
 * agg_core.h - the aggregator's arithmetic, and the sampler's synthetic
 * source. Header-only, no ESP-IDF includes: the same file is compiled by
 * main/aggregator.c on target and by test/test_agg.c under plain cc, so
 * every branch here is exercised on the host before it ever runs under
 * the scheduler (02's pattern: the pure logic is host-tested, the RTOS
 * wiring is target-tested over serial).
 */
#ifndef AGG_CORE_H
#define AGG_CORE_H

#include <stdint.h>
#include <stdbool.h>

/* One aggregation window in flight. */
typedef struct {
    int32_t  sum;
    int16_t  min;
    int16_t  max;
    uint16_t count;
} agg_window_t;

/* One finished window, what the uplink task transmits. */
typedef struct {
    int16_t  min;
    int16_t  max;
    int16_t  mean;
    uint16_t count;
} agg_result_t;

static inline void agg_reset(agg_window_t *w)
{
    w->sum   = 0;
    w->count = 0;
    w->min   = INT16_MAX;
    w->max   = INT16_MIN;
}

static inline void agg_add(agg_window_t *w, int16_t sample)
{
    w->sum += sample;
    if (sample < w->min) w->min = sample;
    if (sample > w->max) w->max = sample;
    w->count++;
}

/*
 * Close a window. Returns false (and writes nothing) on an empty window -
 * the aggregator must not publish a result made of INT16_MAX/INT16_MIN
 * sentinels. Mean rounds half away from zero so +/-  inputs are treated
 * symmetrically (plain C division truncates toward zero, which would bias
 * negative means upward).
 */
static inline bool agg_finalize(const agg_window_t *w, agg_result_t *out)
{
    if (w->count == 0) return false;
    int32_t half = (int32_t)w->count / 2;
    out->min   = w->min;
    out->max   = w->max;
    out->mean  = (int16_t)((w->sum >= 0) ? (w->sum + half) / w->count
                                         : (w->sum - half) / w->count);
    out->count = w->count;
    return true;
}

/*
 * Samples cross the stream buffer as 2 bytes, little-endian, so both ends
 * agree on layout without sharing a struct (and the host tests can pin it).
 */
static inline void agg_encode_sample(int16_t s, uint8_t b[2])
{
    b[0] = (uint8_t)(s & 0xff);
    b[1] = (uint8_t)((s >> 8) & 0xff);
}

static inline int16_t agg_decode_sample(const uint8_t b[2])
{
    return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

/*
 * The synthetic "ADC": a +/-1000 triangle wave with a 200-sample period
 * plus a small deterministic dither, so the pipeline has real min/max/mean
 * structure and the host tests can compute the expected aggregate in
 * closed form. n is the sample index since boot.
 */
static inline int16_t agg_synth_sample(uint32_t n)
{
    uint32_t phase = n % 200;
    int32_t tri = (phase < 100) ? ((int32_t)phase * 20 - 1000)
                                : (1000 - (int32_t)(phase - 100) * 20);
    int32_t dither = (int32_t)((n * 31u) % 7u) - 3;
    return (int16_t)(tri + dither);
}

#endif /* AGG_CORE_H */
