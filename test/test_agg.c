/*
 * test_agg.c - host-native tests for main/agg_core.h: the window fold,
 * the mean rounding, the wire encoding, and the synthetic source. Runs
 * under plain cc in under a second; the coverage stage requires every
 * branch in agg_core.h taken both ways.
 */
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#include "../main/agg_core.h"

static int checks = 0;

#define CHECK(cond) do {                                              \
        checks++;                                                     \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n",                       \
                    __FILE__, __LINE__, #cond);                       \
            exit(1);                                                  \
        }                                                             \
    } while (0)

static void test_empty_window(void)
{
    agg_window_t w;
    agg_result_t r = { .min = 42, .max = 42, .mean = 42, .count = 42 };
    agg_reset(&w);
    CHECK(!agg_finalize(&w, &r));
    CHECK(r.min == 42 && r.max == 42 && r.mean == 42 && r.count == 42);
}

static void test_single_sample(void)
{
    agg_window_t w;
    agg_result_t r;
    agg_reset(&w);
    agg_add(&w, -7);
    CHECK(agg_finalize(&w, &r));
    CHECK(r.min == -7 && r.max == -7 && r.mean == -7 && r.count == 1);
}

static void test_min_max_tracking(void)
{
    /* Ascending then descending so both comparisons in agg_add go both
     * ways: ascending keeps taking the max branch and skipping min,
     * descending the reverse. */
    agg_window_t w;
    agg_result_t r;
    agg_reset(&w);
    for (int16_t s = -3; s <= 3; s++) agg_add(&w, s);
    for (int16_t s = 2; s >= -2; s--) agg_add(&w, s);
    CHECK(agg_finalize(&w, &r));
    CHECK(r.min == -3 && r.max == 3 && r.count == 12);
    CHECK(r.mean == 0);
}

static void test_mean_rounds_away_from_zero(void)
{
    agg_window_t w;
    agg_result_t r;

    /* sum 3 over 2 samples: truncation says 1, half-away-from-zero says 2 */
    agg_reset(&w);
    agg_add(&w, 1);
    agg_add(&w, 2);
    CHECK(agg_finalize(&w, &r) && r.mean == 2);

    /* mirrored: sum -3 over 2 must give -2, not the upward-biased -1 */
    agg_reset(&w);
    agg_add(&w, -1);
    agg_add(&w, -2);
    CHECK(agg_finalize(&w, &r) && r.mean == -2);
}

static void test_extremes_do_not_overflow(void)
{
    /* A window of full-scale samples: the int32 sum must carry it. */
    agg_window_t w;
    agg_result_t r;
    agg_reset(&w);
    for (int i = 0; i < 1000; i++) agg_add(&w, INT16_MAX);
    CHECK(agg_finalize(&w, &r));
    CHECK(r.min == INT16_MAX && r.max == INT16_MAX && r.mean == INT16_MAX);

    agg_reset(&w);
    for (int i = 0; i < 1000; i++) agg_add(&w, INT16_MIN);
    CHECK(agg_finalize(&w, &r));
    CHECK(r.min == INT16_MIN && r.max == INT16_MIN && r.mean == INT16_MIN);
}

static void test_window_reuse(void)
{
    /* reset must fully clear the previous window's state */
    agg_window_t w;
    agg_result_t r;
    agg_reset(&w);
    agg_add(&w, 1000);
    agg_add(&w, -1000);
    CHECK(agg_finalize(&w, &r) && r.count == 2);
    agg_reset(&w);
    agg_add(&w, 5);
    CHECK(agg_finalize(&w, &r));
    CHECK(r.min == 5 && r.max == 5 && r.mean == 5 && r.count == 1);
}

static void test_wire_encoding(void)
{
    const int16_t cases[] = { 0, 1, -1, 999, -999, INT16_MAX, INT16_MIN };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t b[2];
        agg_encode_sample(cases[i], b);
        CHECK(agg_decode_sample(b) == cases[i]);
    }
    /* pin the layout, not just the round trip: little-endian on the wire */
    uint8_t b[2];
    agg_encode_sample(0x1234, b);
    CHECK(b[0] == 0x34 && b[1] == 0x12);
}

static void test_synth_source(void)
{
    /* Periodicity: the source repeats every lcm(200, 7) = 1400 samples
     * (triangle period 200, dither period 7). */
    for (uint32_t n = 0; n < 300; n++)
        CHECK(agg_synth_sample(n) == agg_synth_sample(n + 1400));

    /* Bounds: +/-1000 triangle, dither in [-3, +3]. */
    for (uint32_t n = 0; n < 1400; n++) {
        int16_t s = agg_synth_sample(n);
        CHECK(s >= -1003 && s <= 1003);
    }

    /* One full triangle period through the fold, exactly as the firmware
     * pipes it (encode -> stream -> decode -> add): the wave must show
     * near-full swing and a near-zero mean. */
    agg_window_t w;
    agg_result_t r;
    agg_reset(&w);
    for (uint32_t n = 0; n < 200; n++) {
        uint8_t b[2];
        agg_encode_sample(agg_synth_sample(n), b);
        agg_add(&w, agg_decode_sample(b));
    }
    CHECK(agg_finalize(&w, &r));
    CHECK(r.count == 200);
    CHECK(r.min <= -995 && r.min >= -1003);
    CHECK(r.max >= 995 && r.max <= 1003);
    CHECK(r.mean >= -5 && r.mean <= 5);
}

int main(void)
{
    test_empty_window();
    test_single_sample();
    test_min_max_tracking();
    test_mean_rounds_away_from_zero();
    test_extremes_do_not_overflow();
    test_window_reuse();
    test_wire_encoding();
    test_synth_source();
    printf("test_agg: %d checks passed\n", checks);
    return 0;
}
