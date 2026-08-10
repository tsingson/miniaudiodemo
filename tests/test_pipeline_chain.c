#include "play_pipeline.h"
#include "play_pipeline_eq_stages.h"
#include "play_dsp_common.h"
#include "play_eq_normal.h"
#include "play_multiband_dynamics.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
    float gain;
} stage_gain_ctx;

static float block_read_sample(const play_frame_block* block, uint32_t frame, uint32_t ch)
{
    if (block->layout == PLAY_BUFFER_LAYOUT_PLANAR)
    {
        return block->planar[ch][frame];
    }
    return block->interleaved[frame * block->channels + ch];
}

static void block_write_sample(play_frame_block* block, uint32_t frame, uint32_t ch, float value)
{
    if (block->layout == PLAY_BUFFER_LAYOUT_PLANAR)
    {
        block->planar[ch][frame] = value;
        return;
    }
    block->interleaved[frame * block->channels + ch] = value;
}

static int stage_gain_mul(void* ctx, play_frame_block* block)
{
    stage_gain_ctx* g = (stage_gain_ctx*)ctx;
    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < block->channels; ++ch)
        {
            float v = block_read_sample(block, i, ch);
            block_write_sample(block, i, ch, v * g->gain);
        }
    }
    return 0;
}

static int stage_add_one(void* ctx, play_frame_block* block)
{
    (void)ctx;
    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < block->channels; ++ch)
        {
            float v = block_read_sample(block, i, ch);
            block_write_sample(block, i, ch, v + 1.0f);
        }
    }
    return 0;
}

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

static void test_stage_order_regression(void)
{
    float bufA[2] = {1.0f, 1.0f};
    float bufB[2] = {1.0f, 1.0f};
    play_frame_block blockA;
    play_frame_block blockB;
    play_pipeline pa;
    play_pipeline pb;
    stage_gain_ctx g;

    g.gain = 2.0f;

    memset(&blockA, 0, sizeof(blockA));
    memset(&blockB, 0, sizeof(blockB));

    blockA.interleaved = bufA;
    blockA.layout = PLAY_BUFFER_LAYOUT_INTERLEAVED;
    blockA.frameCount = 1;
    blockA.channels = 2;
    blockA.channelMask = PLAY_DSP_CHANNEL_BOTH;
    blockA.sampleRate = 48000;
    blockB = blockA;
    blockB.interleaved = bufB;

    play_pipeline_init(&pa);
    play_pipeline_add_stage(&pa, "mul2", 1, &g, stage_gain_mul);
    play_pipeline_add_stage(&pa, "add1", 1, NULL, stage_add_one);
    play_pipeline_run(&pa, &blockA);

    play_pipeline_init(&pb);
    play_pipeline_add_stage(&pb, "add1", 1, NULL, stage_add_one);
    play_pipeline_add_stage(&pb, "mul2", 1, &g, stage_gain_mul);
    play_pipeline_run(&pb, &blockB);

    CHECK_NEAR(bufA[0], 3.0f, 1e-6, "order_mul_then_add");
    CHECK_NEAR(bufB[0], 4.0f, 1e-6, "order_add_then_mul");
}

static void test_planar_stage_order_regression(void)
{
    float leftA[1] = {1.0f};
    float rightA[1] = {1.0f};
    float leftB[1] = {1.0f};
    float rightB[1] = {1.0f};
    float* planarA[2] = {leftA, rightA};
    float* planarB[2] = {leftB, rightB};
    play_frame_block blockA;
    play_frame_block blockB;
    play_pipeline pa;
    play_pipeline pb;
    stage_gain_ctx g;

    g.gain = 2.0f;

    memset(&blockA, 0, sizeof(blockA));
    memset(&blockB, 0, sizeof(blockB));

    blockA.planar = planarA;
    blockA.layout = PLAY_BUFFER_LAYOUT_PLANAR;
    blockA.frameCount = 1;
    blockA.channels = 2;
    blockA.channelMask = PLAY_DSP_CHANNEL_BOTH;
    blockA.sampleRate = 48000;

    blockB = blockA;
    blockB.planar = planarB;

    play_pipeline_init(&pa);
    play_pipeline_add_stage(&pa, "mul2", 1, &g, stage_gain_mul);
    play_pipeline_add_stage(&pa, "add1", 1, NULL, stage_add_one);
    play_pipeline_run(&pa, &blockA);

    play_pipeline_init(&pb);
    play_pipeline_add_stage(&pb, "add1", 1, NULL, stage_add_one);
    play_pipeline_add_stage(&pb, "mul2", 1, &g, stage_gain_mul);
    play_pipeline_run(&pb, &blockB);

    CHECK_NEAR(leftA[0], 3.0f, 1e-6, "planar_order_mul_then_add_L");
    CHECK_NEAR(rightA[0], 3.0f, 1e-6, "planar_order_mul_then_add_R");
    CHECK_NEAR(leftB[0], 4.0f, 1e-6, "planar_order_add_then_mul_L");
    CHECK_NEAR(rightB[0], 4.0f, 1e-6, "planar_order_add_then_mul_R");
}

static void test_crossfeed_mbdyn_stage_gates(void)
{
    play_dsp_state dsp;
    play_crossfeed_stage preCross;
    play_crossfeed_stage postCross;
    play_mbdyn_stage preMb;
    play_mbdyn_stage postMb;
    play_pipeline p;
    play_frame_block b;
    float x[2] = {1.0f, 0.0f};
    float amounts[PLAY_MB_DYN_BANDS] = {0};
    float baselineR;

    memset(&dsp, 0, sizeof(dsp));
    dsp.sampleRate = 48000;
    dsp.channels = 2;
    dsp.lowCrossfeedLpA = expf(-2.0f * (float)M_PI * PLAY_LOW_CROSSFEED_CUTOFF_HZ / (float)dsp.sampleRate);
    play_mb_dyn_init(&dsp);

    play_crossfeed_stage_init(&preCross, &dsp, 1, (uint8_t)PLAY_CROSSFEED_PRE_EQ, PLAY_LOW_CROSSFEED_RATIO);
    play_crossfeed_stage_init(&postCross, &dsp, 1, (uint8_t)PLAY_CROSSFEED_POST_EQ, PLAY_LOW_CROSSFEED_RATIO);
    play_mbdyn_stage_init(&preMb,
                          &dsp,
                          1,
                          (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                          dsp.mbDynThreshold,
                          dsp.mbDynAttack,
                          dsp.mbDynRelease,
                          dsp.mbDynStrength);
    play_mbdyn_stage_init(&postMb,
                          &dsp,
                          1,
                          (uint8_t)PLAY_CROSSFEED_POST_EQ,
                          dsp.mbDynThreshold,
                          dsp.mbDynAttack,
                          dsp.mbDynRelease,
                          dsp.mbDynStrength);

    play_pipeline_init(&p);
    play_pipeline_add_stage(&p, "pre-cross", 1, &preCross, play_crossfeed_stage_process);
    play_pipeline_add_stage(&p, "post-cross", 1, &postCross, play_crossfeed_stage_process);

    b.interleaved = x;
    b.layout = PLAY_BUFFER_LAYOUT_INTERLEAVED;
    b.frameCount = 1;
    b.channels = 2;
    b.channelMask = PLAY_DSP_CHANNEL_BOTH;
    b.sampleRate = 48000;

    dsp.lowCrossfeedEnabled = 1u;
    dsp.lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    x[0] = 1.0f;
    x[1] = 0.0f;
    play_pipeline_run(&p, &b);
    baselineR = x[1];
    CHECK_TRUE(fabsf(baselineR) > 1e-5f, "crossfeed_pre_stage_applies", "");

    x[0] = 1.0f;
    x[1] = 0.0f;
    play_pipeline_set_stage_enabled(&p, 0, 0);
    play_pipeline_run(&p, &b);
    CHECK_NEAR(x[1], 0.0f, 1e-6, "crossfeed_stage_disable_passthrough");

    play_pipeline_set_stage_enabled(&p, 0, 1);
    dsp.lowCrossfeedEnabled = 1u;
    dsp.lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_POST_EQ;
    x[0] = 1.0f;
    x[1] = 0.0f;
    play_pipeline_run(&p, &b);
    CHECK_TRUE(fabsf(x[1]) > 1e-5f, "crossfeed_post_stage_applies", "");

    memset(amounts, 0, sizeof(amounts));
    amounts[0] = 6.0f;
    play_mb_dyn_set_amounts(&dsp, amounts, PLAY_MB_DYN_BANDS);
    play_mb_dyn_set_config(&dsp,
                           1,
                           (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                           0.01f,
                           PLAY_MB_DYN_MAX_ATTACK,
                           PLAY_MB_DYN_MIN_RELEASE,
                           PLAY_MB_DYN_MAX_STRENGTH);

    play_pipeline_init(&p);
    play_pipeline_add_stage(&p, "pre-mb", 1, &preMb, play_mbdyn_stage_process);
    play_pipeline_add_stage(&p, "post-mb", 1, &postMb, play_mbdyn_stage_process);

    {
        float accum = 0.0f;
        for (int i = 0; i < 64; ++i)
        {
            x[0] = 0.9f;
            x[1] = 0.9f;
            play_pipeline_run(&p, &b);
            accum += fabsf(x[0] - 0.9f);
        }
        CHECK_TRUE(accum > 1e-4f, "mbdyn_pre_stage_applies", "");
    }

    play_pipeline_set_stage_enabled(&p, 0, 0);
    x[0] = 0.9f;
    x[1] = 0.9f;
    play_pipeline_run(&p, &b);
    CHECK_NEAR(x[0], 0.9f, 1e-6, "mbdyn_stage_disable_passthrough");
}

static void test_planar_crossfeed_mbdyn_stage_gates(void)
{
    play_dsp_state dsp;
    play_crossfeed_stage preCross;
    play_crossfeed_stage postCross;
    play_mbdyn_stage preMb;
    play_mbdyn_stage postMb;
    play_pipeline p;
    play_frame_block b;
    float left[1] = {1.0f};
    float right[1] = {0.0f};
    float* planar[2] = {left, right};
    float amounts[PLAY_MB_DYN_BANDS] = {0};
    float baselineR;

    memset(&dsp, 0, sizeof(dsp));
    dsp.sampleRate = 48000;
    dsp.channels = 2;
    dsp.lowCrossfeedLpA = expf(-2.0f * (float)M_PI * PLAY_LOW_CROSSFEED_CUTOFF_HZ / (float)dsp.sampleRate);
    play_mb_dyn_init(&dsp);

    play_crossfeed_stage_init(&preCross, &dsp, 1, (uint8_t)PLAY_CROSSFEED_PRE_EQ, PLAY_LOW_CROSSFEED_RATIO);
    play_crossfeed_stage_init(&postCross, &dsp, 1, (uint8_t)PLAY_CROSSFEED_POST_EQ, PLAY_LOW_CROSSFEED_RATIO);
    play_mbdyn_stage_init(&preMb,
                          &dsp,
                          1,
                          (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                          dsp.mbDynThreshold,
                          dsp.mbDynAttack,
                          dsp.mbDynRelease,
                          dsp.mbDynStrength);
    play_mbdyn_stage_init(&postMb,
                          &dsp,
                          1,
                          (uint8_t)PLAY_CROSSFEED_POST_EQ,
                          dsp.mbDynThreshold,
                          dsp.mbDynAttack,
                          dsp.mbDynRelease,
                          dsp.mbDynStrength);

    memset(&b, 0, sizeof(b));
    b.planar = planar;
    b.layout = PLAY_BUFFER_LAYOUT_PLANAR;
    b.frameCount = 1;
    b.channels = 2;
    b.channelMask = PLAY_DSP_CHANNEL_BOTH;
    b.sampleRate = 48000;

    play_pipeline_init(&p);
    play_pipeline_add_stage(&p, "pre-cross", 1, &preCross, play_crossfeed_stage_process);
    play_pipeline_add_stage(&p, "post-cross", 1, &postCross, play_crossfeed_stage_process);

    dsp.lowCrossfeedEnabled = 1u;
    dsp.lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    left[0] = 1.0f;
    right[0] = 0.0f;
    play_pipeline_run(&p, &b);
    baselineR = right[0];
    CHECK_TRUE(fabsf(baselineR) > 1e-5f, "planar_crossfeed_pre_stage_applies", "");

    left[0] = 1.0f;
    right[0] = 0.0f;
    play_pipeline_set_stage_enabled(&p, 0, 0);
    play_pipeline_run(&p, &b);
    CHECK_NEAR(right[0], 0.0f, 1e-6, "planar_crossfeed_stage_disable_passthrough");

    play_pipeline_set_stage_enabled(&p, 0, 1);
    dsp.lowCrossfeedEnabled = 1u;
    dsp.lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_POST_EQ;
    left[0] = 1.0f;
    right[0] = 0.0f;
    play_pipeline_run(&p, &b);
    CHECK_TRUE(fabsf(right[0]) > 1e-5f, "planar_crossfeed_post_stage_applies", "");

    memset(amounts, 0, sizeof(amounts));
    amounts[0] = 6.0f;
    play_mb_dyn_set_amounts(&dsp, amounts, PLAY_MB_DYN_BANDS);
    play_mb_dyn_set_config(&dsp,
                           1,
                           (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                           0.01f,
                           PLAY_MB_DYN_MAX_ATTACK,
                           PLAY_MB_DYN_MIN_RELEASE,
                           PLAY_MB_DYN_MAX_STRENGTH);

    play_pipeline_init(&p);
    play_pipeline_add_stage(&p, "pre-mb", 1, &preMb, play_mbdyn_stage_process);
    play_pipeline_add_stage(&p, "post-mb", 1, &postMb, play_mbdyn_stage_process);

    {
        float accum = 0.0f;
        for (int i = 0; i < 64; ++i)
        {
            left[0] = 0.9f;
            right[0] = 0.9f;
            play_pipeline_run(&p, &b);
            accum += fabsf(left[0] - 0.9f);
        }
        CHECK_TRUE(accum > 1e-4f, "planar_mbdyn_pre_stage_applies", "");
    }

    play_pipeline_set_stage_enabled(&p, 0, 0);
    left[0] = 0.9f;
    right[0] = 0.9f;
    play_pipeline_run(&p, &b);
    CHECK_NEAR(left[0], 0.9f, 1e-6, "planar_mbdyn_stage_disable_passthrough");
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
    block.layout = PLAY_BUFFER_LAYOUT_INTERLEAVED;
    block.frameCount = FRAMES;
    block.channels = CHANNELS;
    block.channelMask = PLAY_DSP_CHANNEL_BOTH;
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

    test_stage_order_regression();
    test_planar_stage_order_regression();
    test_crossfeed_mbdyn_stage_gates();
    test_planar_crossfeed_mbdyn_stage_gates();

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
