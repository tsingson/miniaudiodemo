#include "play_pipeline.h"
#include "play_pipeline_eq_stages.h"
#include "play_eq_normal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

#define CHECK_TRUE(cond, name, detail) \
    do \
    { \
        if (!(cond)) \
        { \
            printf("FAIL %s %s\\n", name, detail); \
            g_fail++; \
        } \
        else \
        { \
            printf("PASS %s\\n", name); \
        } \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, name) \
    do \
    { \
        double _a = (double)(actual); \
        double _e = (double)(expected); \
        double _d = fabs(_a - _e); \
        if (_d > (tol)) \
        { \
            printf("FAIL %s actual=%.6f expected=%.6f diff=%.6f tol=%.6f\\n", name, _a, _e, _d, (double)(tol)); \
            g_fail++; \
        } \
        else \
        { \
            printf("PASS %s actual=%.6f expected=%.6f\\n", name, _a, _e); \
        } \
    } while (0)

static void fill_sine(float* buf, int frames, int channels, float freq, float sr, float amp)
{
    double phase = 0.0;
    double step = 2.0 * M_PI * (double)freq / (double)sr;

    for (int i = 0; i < frames; ++i)
    {
        float s = amp * (float)sin(phase);
        for (int ch = 0; ch < channels; ++ch)
        {
            buf[i * channels + ch] = s;
        }
        phase += step;
        if (phase > 2.0 * M_PI)
        {
            phase -= 2.0 * M_PI;
        }
    }
}

static float rms(const float* buf, int count)
{
    double sum = 0.0;
    for (int i = 0; i < count; ++i)
    {
        sum += (double)buf[i] * (double)buf[i];
    }
    return (float)sqrt(sum / (double)count);
}

int main(void)
{
    enum
    {
        FRAMES = 512,
        CHANNELS = 1,
        SAMPLE_RATE = 48000
    };

    float inBuf[FRAMES * CHANNELS];
    float workBuf[FRAMES * CHANNELS];
    float freqs[EQ_BANDS];
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    uint8_t modes[EQ_BANDS];

    play_frame_block block;
    play_pipeline pipeline;
    play_eq_stage_profile profile;
    play_linear_eq_stage linearStage;
    play_dynamic_eq_stage dynamicStage;
    play_normal_eq_stage normalStage;

    fill_sine(inBuf, FRAMES, CHANNELS, 700.0f, (float)SAMPLE_RATE, 0.1f);
    memcpy(workBuf, inBuf, sizeof(workBuf));

    play_eq_normal_init_default_profile_arrays(freqs, gains, qs, EQ_BANDS);
    play_eq_assign_band_modes_arrays(freqs,
                                     modes,
                                     EQ_BANDS,
                                     EQ_LINEAR_MAX_HZ,
                                     EQ_DYNAMIC_MAX_HZ,
                                     (uint8_t)PLAY_EQ_MODE_LINEAR,
                                     (uint8_t)PLAY_EQ_MODE_DYNAMIC,
                                     (uint8_t)PLAY_EQ_MODE_NORMAL);

    for (int i = 0; i < EQ_BANDS; ++i)
    {
        gains[i] = 0.0f;
    }
    gains[0] = 3.0f;
    gains[6] = 3.0f;
    gains[15] = -6.0f;

    play_eq_stage_profile_init(&profile,
                               SAMPLE_RATE,
                               CHANNELS,
                               freqs,
                               gains,
                               qs,
                               modes,
                               EQ_BANDS);

    play_linear_eq_stage_init(&linearStage, &profile, 1, 25, (uint8_t)PLAY_EQ_MODE_LINEAR);
    play_dynamic_eq_stage_init(&dynamicStage,
                               SAMPLE_RATE,
                               CHANNELS,
                               freqs,
                               gains,
                               qs,
                               modes,
                               EQ_BANDS,
                               (uint8_t)PLAY_EQ_MODE_LINEAR,
                               (uint8_t)PLAY_EQ_MODE_DYNAMIC);
    play_dynamic_eq_stage_set_params(&dynamicStage,
                                     DYNAMIC_EQ_MIN_ATTACK,
                                     DYNAMIC_EQ_MIN_RELEASE,
                                     DYNAMIC_EQ_MIN_THRESHOLD,
                                     0.0f,
                                     DYNAMIC_EQ_MIN_STRENGTH_DB);
    play_normal_eq_stage_init(&normalStage,
                              SAMPLE_RATE,
                              CHANNELS,
                              freqs,
                              gains,
                              qs,
                              modes,
                              EQ_BANDS,
                              (uint8_t)PLAY_EQ_MODE_LINEAR,
                              (uint8_t)PLAY_EQ_MODE_NORMAL);

    play_pipeline_init(&pipeline);
    CHECK_TRUE(play_pipeline_add_stage(&pipeline, "linear", 1, &linearStage, play_linear_eq_stage_process) >= 0,
               "pipeline_add_linear",
               "");
    CHECK_TRUE(play_pipeline_add_stage(&pipeline, "dynamic", 1, &dynamicStage, play_dynamic_eq_stage_process) >= 0,
               "pipeline_add_dynamic",
               "");
    CHECK_TRUE(play_pipeline_add_stage(&pipeline, "normal", 1, &normalStage, play_normal_eq_stage_process) >= 0,
               "pipeline_add_normal",
               "");

    block.interleaved = workBuf;
    block.frameCount = FRAMES;
    block.channels = CHANNELS;
    block.sampleRate = SAMPLE_RATE;

    CHECK_TRUE(play_pipeline_run(&pipeline, &block) == 0, "pipeline_run_ok", "");

    {
        float inRms = rms(inBuf, FRAMES * CHANNELS);
        float outRms = rms(workBuf, FRAMES * CHANNELS);
        CHECK_TRUE(isfinite(outRms), "pipeline_output_finite", "");
        CHECK_TRUE(fabsf(outRms - inRms) > 1e-5f, "pipeline_changes_signal", "");
    }

    memcpy(workBuf, inBuf, sizeof(workBuf));
    CHECK_TRUE(play_pipeline_set_stage_enabled(&pipeline, 0, 0) == 0, "disable_linear", "");
    CHECK_TRUE(play_pipeline_set_stage_enabled(&pipeline, 1, 0) == 0, "disable_dynamic", "");
    CHECK_TRUE(play_pipeline_set_stage_enabled(&pipeline, 2, 0) == 0, "disable_normal", "");
    CHECK_TRUE(play_pipeline_run(&pipeline, &block) == 0, "pipeline_run_all_disabled", "");
    CHECK_NEAR(rms(workBuf, FRAMES * CHANNELS), rms(inBuf, FRAMES * CHANNELS), 1e-7, "pipeline_all_disabled_passthrough");

    memcpy(workBuf, inBuf, sizeof(workBuf));
    play_pipeline_set_bypass(&pipeline, 1);
    CHECK_TRUE(play_pipeline_run(&pipeline, &block) == 0, "pipeline_run_bypass", "");
    CHECK_NEAR(rms(workBuf, FRAMES * CHANNELS), rms(inBuf, FRAMES * CHANNELS), 1e-7, "pipeline_bypass_passthrough");

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
