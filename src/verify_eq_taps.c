#include "play_dsp_common.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAP_TEST_SR 48000u
#define TAP_TEST_CH 1u
#define TAP_TEST_FRAMES 2048
#define TAP_EPS 1e-6f

static float input_signal(int n)
{
    double w = 2.0 * M_PI * 1000.0 / (double)TAP_TEST_SR;
    return 0.1f * (float)sin((double)n * w);
}

static int test_tap_timeliness_and_completeness(void)
{
    play_dsp_state dsp;
    uint64_t lastSeq = 0;
    float inBuf[ANALYZER_WINDOW];
    float outBuf[ANALYZER_WINDOW];
    int writeIndex = 0;
    uint64_t seq = 0;

    if (play_dsp_init(&dsp, TAP_TEST_SR, TAP_TEST_CH) != 0)
    {
        return 1;
    }

    for (int i = 0; i < TAP_TEST_FRAMES; ++i)
    {
        float x = input_signal(i);
        float frame[1];
        frame[0] = x;
        play_dsp_process(&dsp, frame, 1, TAP_TEST_CH);

        play_dsp_copy_eq_taps(&dsp, inBuf, outBuf, ANALYZER_WINDOW, &writeIndex, &seq);

        if (seq != (uint64_t)(i + 1))
        {
            printf("timeliness FAIL: frame=%d expected_seq=%d got=%llu\n", i, i + 1, (unsigned long long)seq);
            return 1;
        }
        if (i > 0 && seq != lastSeq + 1u)
        {
            printf("completeness FAIL: non-contiguous seq at frame=%d\n", i);
            return 1;
        }
        if (writeIndex != ((i + 1) % ANALYZER_WINDOW))
        {
            printf("timeliness FAIL: writeIndex mismatch frame=%d expected=%d got=%d\n",
                   i,
                   (i + 1) % ANALYZER_WINDOW,
                   writeIndex);
            return 1;
        }

        lastSeq = seq;
    }

    printf("case_tap_timeliness_completeness PASS\n");
    return 0;
}

static int test_tap_accuracy_bypass(void)
{
    play_dsp_state dsp;
    float inBuf[ANALYZER_WINDOW];
    float outBuf[ANALYZER_WINDOW];
    int writeIndex = 0;
    uint64_t seq = 0;

    if (play_dsp_init(&dsp, TAP_TEST_SR, TAP_TEST_CH) != 0)
    {
        return 1;
    }

    play_dsp_set_bypass(&dsp, 1);

    for (int i = 0; i < TAP_TEST_FRAMES; ++i)
    {
        float x = input_signal(i);
        float frame[1];
        int last;
        frame[0] = x;
        play_dsp_process(&dsp, frame, 1, TAP_TEST_CH);
        play_dsp_copy_eq_taps(&dsp, inBuf, outBuf, ANALYZER_WINDOW, &writeIndex, &seq);

        last = (writeIndex - 1 + ANALYZER_WINDOW) % ANALYZER_WINDOW;
        if (fabsf(inBuf[last] - x) > TAP_EPS)
        {
            printf("accuracy FAIL: eq_in mismatch at frame=%d expected=%.7f got=%.7f\n", i, x, inBuf[last]);
            return 1;
        }
        if (fabsf(outBuf[last] - frame[0]) > TAP_EPS)
        {
            printf("accuracy FAIL: eq_out mismatch at frame=%d expected=%.7f got=%.7f\n", i, frame[0], outBuf[last]);
            return 1;
        }
    }

    printf("case_tap_accuracy_bypass PASS\n");
    return 0;
}

int main(void)
{
    int fail = 0;

    fail += test_tap_timeliness_and_completeness();
    fail += test_tap_accuracy_bypass();

    printf("summary: failed=%d\n", fail);
    return (fail == 0) ? 0 : 1;
}
