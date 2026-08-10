#include "play_dsp_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_SAMPLE_RATE 48000u
#define TEST_CHANNELS 1u
#define WARMUP_FRAMES 8192
#define MEASURE_FRAMES 16384
#define TEST_TOLERANCE_DB 1.2f

static float rms_for_tone(play_dsp_state* dsp, float freqHz, float* outPostRms)
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

static void clear_classic_eq(play_dsp_state* dsp)
{
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];

    if (dsp == NULL)
    {
        return;
    }

    memset(gains, 0, sizeof(gains));
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        qs[i] = 0.9f;
    }
    play_dsp_set_eq_params(dsp, gains, EQ_BANDS, qs, EQ_BANDS);
    play_dsp_set_dynamic_params(dsp,
                                DYNAMIC_EQ_MIN_ATTACK,
                                DYNAMIC_EQ_MIN_RELEASE,
                                DYNAMIC_EQ_MIN_THRESHOLD,
                                0.0f,
                                DYNAMIC_EQ_MIN_STRENGTH_DB);
    play_dsp_set_multiband_dynamics_config(dsp,
                                           0,
                                           (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                                           PLAY_MB_DYN_MIN_THRESHOLD,
                                           PLAY_MB_DYN_MIN_ATTACK,
                                           PLAY_MB_DYN_MIN_RELEASE,
                                           PLAY_MB_DYN_MIN_STRENGTH);

    play_dsp_set_low_crossfeed(dsp, 0, (uint8_t)PLAY_CROSSFEED_PRE_EQ);
    (void)play_dsp_plugin_set_bypass(dsp, (uint8_t)PLAY_PLUGIN_EQ_CORE, 1);
    (void)play_dsp_plugin_set_bypass(dsp, (uint8_t)PLAY_PLUGIN_CROSSFEED_PRE, 1);
    (void)play_dsp_plugin_set_bypass(dsp, (uint8_t)PLAY_PLUGIN_CROSSFEED_POST, 1);
    (void)play_dsp_plugin_set_bypass(dsp, (uint8_t)PLAY_PLUGIN_MBDYN_PRE, 1);
    (void)play_dsp_plugin_set_bypass(dsp, (uint8_t)PLAY_PLUGIN_MBDYN_POST, 1);
}

static int run_low_seq_case(float freqHz, float targetDb)
{
    static const float kBandsHz[5] = {35.0f, 60.0f, 110.0f, 220.0f, 300.0f};
    static const float kBandsQ[5] = {2.2f, 2.1f, 1.8f, 1.4f, 1.2f};
    play_dsp_state dsp;
    float inRms;
    float outRms;
    float measuredDb;
    float err;

    if (play_dsp_init(&dsp, TEST_SAMPLE_RATE, TEST_CHANNELS) != 0)
    {
        fprintf(stderr, "init failed\n");
        return 1;
    }

    clear_classic_eq(&dsp);
    play_dsp_set_low_seq_mode(&dsp, EQ_LOW_SEQ_MODE_BALANCED);
    play_dsp_set_low_seq_enabled(&dsp, 1);
    (void)play_dsp_plugin_set_bypass(&dsp, (uint8_t)PLAY_PLUGIN_LOW_SEQ, 0);

    for (int i = 0; i < 5; ++i)
    {
        float g = (fabsf(kBandsHz[i] - freqHz) < 0.1f) ? targetDb : 0.0f;
        play_dsp_set_low_seq_band(&dsp, i, kBandsHz[i], g, kBandsQ[i], 1);
    }

    if (play_dsp_plugin_validate(&dsp) != 0)
    {
        fprintf(stderr, "plugin validation failed\n");
        return 1;
    }

    inRms = rms_for_tone(&dsp, freqHz, &outRms);
    measuredDb = 20.0f * log10f((outRms + 1e-12f) / (inRms + 1e-12f));
    err = measuredDb - targetDb;

    printf("low_seq freq=%7.1fHz target=%5.1fdB measured=%7.3fdB err=%+6.3fdB %s\n",
           freqHz,
           targetDb,
           measuredDb,
           err,
           (fabsf(err) <= TEST_TOLERANCE_DB) ? "PASS" : "FAIL");

    return (fabsf(err) <= TEST_TOLERANCE_DB) ? 0 : 1;
}

int main(void)
{
    static const float kTestFreqs[5] = {35.0f, 60.0f, 110.0f, 220.0f, 300.0f};
    static const float kTargetDb[3] = {3.0f, 6.0f, 12.0f};
    int failCount = 0;

    printf("EQ low-seq gain verify: 35/60/110/220/300Hz with +3/+6/+12dB, tolerance=%.2fdB\n",
           (double)TEST_TOLERANCE_DB);

    for (int f = 0; f < 5; ++f)
    {
        for (int g = 0; g < 3; ++g)
        {
            failCount += run_low_seq_case(kTestFreqs[f], kTargetDb[g]);
        }
    }

    printf("summary: total_cases=%d failed=%d\n", 15, failCount);

    return (failCount == 0) ? 0 : 1;
}
