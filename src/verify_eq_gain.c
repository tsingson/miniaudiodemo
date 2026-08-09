#include "play_dsp_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_SAMPLE_RATE 48000u
#define TEST_CHANNELS 1u
#define WARMUP_FRAMES 8192
#define MEASURE_FRAMES 16384
#define TEST_TOLERANCE_DB 0.8

static float rms_for_band(play_dsp_state* dsp, float freqHz, float* outPostRms)
{
    float buf[TEST_CHANNELS];
    double inSum = 0.0;
    double outSum = 0.0;
    double phase = 0.0;
    double phaseStep = 2.0 * M_PI * (double)freqHz / (double)TEST_SAMPLE_RATE;
    const float amp = 0.08f;

    for (int i = 0; i < WARMUP_FRAMES; ++i)
    {
        float in = amp * (float)sin(phase);
        buf[0] = in;
        play_dsp_process(dsp, buf, 1, TEST_CHANNELS);
        phase += phaseStep;
        if (phase >= 2.0 * M_PI)
        {
            phase -= 2.0 * M_PI;
        }
    }

    for (int i = 0; i < MEASURE_FRAMES; ++i)
    {
        float in = amp * (float)sin(phase);
        float out;
        buf[0] = in;
        play_dsp_process(dsp, buf, 1, TEST_CHANNELS);
        out = buf[0];

        inSum += (double)in * (double)in;
        outSum += (double)out * (double)out;

        phase += phaseStep;
        if (phase >= 2.0 * M_PI)
        {
            phase -= 2.0 * M_PI;
        }
    }

    if (outPostRms != NULL)
    {
        *outPostRms = (float)sqrt(outSum / (double)MEASURE_FRAMES);
    }
    return (float)sqrt(inSum / (double)MEASURE_FRAMES);
}

static int run_single_case(float freqHz, int bandIndex, float targetDb)
{
    play_dsp_state dsp;
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    float inRms;
    float outRms;
    float measuredDb;
    float err;

    if (play_dsp_init(&dsp, TEST_SAMPLE_RATE, TEST_CHANNELS) != 0)
    {
        fprintf(stderr, "init failed for band %d\n", bandIndex);
        return 1;
    }

    memset(gains, 0, sizeof(gains));
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        qs[i] = 0.9f;
    }
    gains[bandIndex] = targetDb;

    play_dsp_set_eq_params(&dsp, gains, EQ_BANDS, qs, EQ_BANDS);
    play_dsp_set_dynamic_params(&dsp,
                                DYNAMIC_EQ_MIN_ATTACK,
                                DYNAMIC_EQ_MIN_RELEASE,
                                DYNAMIC_EQ_MIN_THRESHOLD,
                                0.0f,
                                DYNAMIC_EQ_MIN_STRENGTH_DB);
    play_dsp_set_multiband_dynamics_config(&dsp,
                                           0,
                                           (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                                           PLAY_MB_DYN_MIN_THRESHOLD,
                                           PLAY_MB_DYN_MIN_ATTACK,
                                           PLAY_MB_DYN_MIN_RELEASE,
                                           PLAY_MB_DYN_MIN_STRENGTH);

    inRms = rms_for_band(&dsp, freqHz, &outRms);
    measuredDb = 20.0f * log10f((outRms + 1e-12f) / (inRms + 1e-12f));
    err = measuredDb - targetDb;

    printf("band=%2d freq=%7.1fHz target=%5.1fdB measured=%7.3fdB err=%+6.3fdB %s\n",
           bandIndex,
           freqHz,
           targetDb,
           measuredDb,
           err,
           (fabsf(err) <= TEST_TOLERANCE_DB) ? "PASS" : "FAIL");

    return (fabsf(err) <= TEST_TOLERANCE_DB) ? 0 : 1;
}

int main(void)
{
    play_dsp_state probe;
    float freqs[EQ_BANDS];
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    int failCount = 0;
    int testedBands = 0;

    if (play_dsp_init(&probe, TEST_SAMPLE_RATE, TEST_CHANNELS) != 0)
    {
        fprintf(stderr, "failed to init probe dsp\n");
        return 2;
    }

    play_dsp_copy_eq(&probe, freqs, gains, qs, EQ_BANDS);

    printf("EQ verify: low+mid bands, each band at +/-3dB, tolerance=%.2fdB\n", (double)TEST_TOLERANCE_DB);
    printf("mode split: low<=%.1fHz linear, mid<=%.1fHz dynamic\n", (double)EQ_LINEAR_MAX_HZ, (double)EQ_DYNAMIC_MAX_HZ);

    for (int i = 0; i < EQ_BANDS; ++i)
    {
        float f = freqs[i];
        if (f <= EQ_DYNAMIC_MAX_HZ)
        {
            testedBands += 1;
            failCount += run_single_case(f, i, 3.0f);
            failCount += run_single_case(f, i, -3.0f);
        }
    }

    printf("summary: tested_bands=%d total_cases=%d failed=%d\n",
           testedBands,
           testedBands * 2,
           failCount);

    return (failCount == 0) ? 0 : 1;
}
