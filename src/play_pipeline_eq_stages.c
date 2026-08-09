#include "play_pipeline_eq_stages.h"

#include "play_eq_normal.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static uint32_t channels_sanitize(uint32_t channels)
{
    if (channels == 0u)
    {
        return 1u;
    }
    if (channels > MAX_CHANNELS)
    {
        return MAX_CHANNELS;
    }
    return channels;
}

static int bands_sanitize(int bandCount)
{
    if (bandCount < 0)
    {
        return 0;
    }
    if (bandCount > EQ_BANDS)
    {
        return EQ_BANDS;
    }
    return bandCount;
}

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

void play_eq_stage_profile_init(play_eq_stage_profile* profile,
                                uint32_t sampleRate,
                                uint32_t channels,
                                const float* freqs,
                                const float* gains,
                                const float* qs,
                                const uint8_t* modes,
                                int bandCount)
{
    int n;

    if (profile == NULL || freqs == NULL || gains == NULL || qs == NULL || modes == NULL)
    {
        return;
    }

    memset(profile, 0, sizeof(*profile));
    profile->sampleRate = sampleRate;
    profile->channels = channels_sanitize(channels);
    profile->bandCount = bands_sanitize(bandCount);
    n = profile->bandCount;

    for (int i = 0; i < n; ++i)
    {
        profile->freqs[i] = freqs[i];
        profile->gainsDB[i] = gains[i];
        profile->qValues[i] = qs[i];
        profile->modes[i] = modes[i];
    }
}

void play_linear_eq_stage_init(play_linear_eq_stage* stage,
                               const play_eq_stage_profile* profile,
                               int enabled,
                               int taps,
                               uint8_t linearModeValue)
{
    if (stage == NULL || profile == NULL)
    {
        return;
    }

    memset(stage, 0, sizeof(*stage));
    stage->profile = *profile;
    stage->linearModeValue = linearModeValue;

    play_eq_linear_ctx_init(&stage->linear, stage->profile.sampleRate, stage->profile.channels);
    play_eq_linear_ctx_set_config(&stage->linear, enabled, taps);
    play_linear_eq_stage_rebuild(stage);
}

void play_linear_eq_stage_rebuild(play_linear_eq_stage* stage)
{
    if (stage == NULL)
    {
        return;
    }

    play_eq_linear_ctx_rebuild(&stage->linear,
                               stage->profile.freqs,
                               stage->profile.gainsDB,
                               stage->profile.qValues,
                               stage->profile.modes,
                               stage->profile.bandCount,
                               stage->linearModeValue);
}

void play_dynamic_eq_stage_init(play_dynamic_eq_stage* stage,
                                uint32_t sampleRate,
                                uint32_t channels,
                                const float* freqs,
                                const float* gains,
                                const float* qs,
                                const uint8_t* modes,
                                int bandCount,
                                uint8_t linearModeValue,
                                uint8_t dynamicModeValue)
{
    int n;

    if (stage == NULL || freqs == NULL || gains == NULL || qs == NULL || modes == NULL)
    {
        return;
    }

    memset(stage, 0, sizeof(*stage));
    stage->sampleRate = sampleRate;
    stage->channels = channels_sanitize(channels);
    stage->bandCount = bands_sanitize(bandCount);
    stage->linearModeValue = linearModeValue;
    stage->dynamicModeValue = dynamicModeValue;
    n = stage->bandCount;

    for (int i = 0; i < n; ++i)
    {
        stage->gainsDB[i] = gains[i];
        stage->modes[i] = modes[i];
    }

    play_eq_dynamic_init_default_params(&stage->params);
    play_eq_normal_rebuild_arrays(sampleRate,
                                  freqs,
                                  gains,
                                  qs,
                                  modes,
                                  n,
                                  linearModeValue,
                                  EQ_LOW_SVF_MAX_HZ,
                                  stage->useSVF,
                                  stage->b0,
                                  stage->b1,
                                  stage->b2,
                                  stage->a1,
                                  stage->a2,
                                  stage->svfG,
                                  stage->svfK,
                                  stage->svfA,
                                  stage->svfH);
}

void play_dynamic_eq_stage_set_params(play_dynamic_eq_stage* stage,
                                      float attack,
                                      float release,
                                      float threshold,
                                      float maxReductionDB,
                                      float strengthDB)
{
    if (stage == NULL)
    {
        return;
    }

    play_eq_dynamic_set_params(&stage->params, attack, release, threshold, maxReductionDB, strengthDB);
}

void play_normal_eq_stage_init(play_normal_eq_stage* stage,
                               uint32_t sampleRate,
                               uint32_t channels,
                               const float* freqs,
                               const float* gains,
                               const float* qs,
                               const uint8_t* modes,
                               int bandCount,
                               uint8_t linearModeValue,
                               uint8_t normalModeValue)
{
    int n;

    if (stage == NULL || freqs == NULL || gains == NULL || qs == NULL || modes == NULL)
    {
        return;
    }

    memset(stage, 0, sizeof(*stage));
    stage->sampleRate = sampleRate;
    stage->channels = channels_sanitize(channels);
    stage->bandCount = bands_sanitize(bandCount);
    stage->linearModeValue = linearModeValue;
    stage->normalModeValue = normalModeValue;
    n = stage->bandCount;

    for (int i = 0; i < n; ++i)
    {
        stage->modes[i] = modes[i];
    }

    play_eq_normal_rebuild_arrays(sampleRate,
                                  freqs,
                                  gains,
                                  qs,
                                  modes,
                                  n,
                                  linearModeValue,
                                  EQ_LOW_SVF_MAX_HZ,
                                  stage->useSVF,
                                  stage->b0,
                                  stage->b1,
                                  stage->b2,
                                  stage->a1,
                                  stage->a2,
                                  stage->svfG,
                                  stage->svfK,
                                  stage->svfA,
                                  stage->svfH);
}

int play_linear_eq_stage_process(void* ctx, play_frame_block* block)
{
    play_linear_eq_stage* stage = (play_linear_eq_stage*)ctx;
    uint32_t channels;
    uint32_t stride;

    if (stage == NULL || block == NULL || block->interleaved == NULL)
    {
        return -1;
    }

    channels = channels_sanitize(block->channels);
    stride = block->channels;

    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            uint32_t idx = i * stride + ch;
            block->interleaved[idx] = play_eq_linear_ctx_process_sample(&stage->linear, ch, block->interleaved[idx]);
        }
    }

    return 0;
}

int play_dynamic_eq_stage_process(void* ctx, play_frame_block* block)
{
    play_dynamic_eq_stage* stage = (play_dynamic_eq_stage*)ctx;
    uint32_t channels;
    uint32_t stride;

    if (stage == NULL || block == NULL || block->interleaved == NULL)
    {
        return -1;
    }

    channels = channels_sanitize(block->channels);
    stride = block->channels;

    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            uint32_t idx = i * stride + ch;
            float in = block->interleaved[idx];

            for (int band = 0; band < stage->bandCount; ++band)
            {
                double out;

                if (stage->modes[band] != stage->dynamicModeValue)
                {
                    continue;
                }

                if (stage->useSVF[band])
                {
                    out = play_eq_normal_process_svf_sample(stage->svfG[band],
                                                            stage->svfK[band],
                                                            stage->svfA[band],
                                                            stage->svfH[band],
                                                            &stage->svfIc1eq[band][ch],
                                                            &stage->svfIc2eq[band][ch],
                                                            in);
                }
                else
                {
                    out = stage->b0[band] * in + stage->b1[band] * stage->x1[band][ch] + stage->b2[band] * stage->x2[band][ch]
                        - stage->a1[band] * stage->y1[band][ch] - stage->a2[band] * stage->y2[band][ch];

                    stage->x2[band][ch] = stage->x1[band][ch];
                    stage->x1[band][ch] = in;
                    stage->y2[band][ch] = stage->y1[band][ch];
                    stage->y1[band][ch] = out;
                }

                {
                    float env = stage->dynamicEnv[band][ch];
                    float absOut = fabsf((float)out);
                    float reductionDb;

                    env = play_eq_dynamic_update_env(env, absOut, stage->params.attack, stage->params.release);
                    stage->dynamicEnv[band][ch] = env;

                    reductionDb = play_eq_dynamic_compute_reduction(env,
                                                                    stage->params.threshold,
                                                                    stage->params.strengthDB,
                                                                    stage->params.maxReductionDB,
                                                                    stage->gainsDB[band] > 0.0f);
                    if (reductionDb > 0.0f)
                    {
                        out *= db_to_linear(-reductionDb);
                    }
                    stage->dynamicReductionDB[band] = stage->dynamicReductionDB[band] * 0.88f + reductionDb * 0.12f;
                }

                in = (float)out;
            }

            block->interleaved[idx] = in;
        }
    }

    return 0;
}

int play_normal_eq_stage_process(void* ctx, play_frame_block* block)
{
    play_normal_eq_stage* stage = (play_normal_eq_stage*)ctx;
    uint32_t channels;
    uint32_t stride;

    if (stage == NULL || block == NULL || block->interleaved == NULL)
    {
        return -1;
    }

    channels = channels_sanitize(block->channels);
    stride = block->channels;

    for (uint32_t i = 0; i < block->frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            uint32_t idx = i * stride + ch;
            float in = block->interleaved[idx];

            for (int band = 0; band < stage->bandCount; ++band)
            {
                double out;

                if (stage->modes[band] != stage->normalModeValue)
                {
                    continue;
                }

                if (stage->useSVF[band])
                {
                    out = play_eq_normal_process_svf_sample(stage->svfG[band],
                                                            stage->svfK[band],
                                                            stage->svfA[band],
                                                            stage->svfH[band],
                                                            &stage->svfIc1eq[band][ch],
                                                            &stage->svfIc2eq[band][ch],
                                                            in);
                }
                else
                {
                    out = stage->b0[band] * in + stage->b1[band] * stage->x1[band][ch] + stage->b2[band] * stage->x2[band][ch]
                        - stage->a1[band] * stage->y1[band][ch] - stage->a2[band] * stage->y2[band][ch];

                    stage->x2[band][ch] = stage->x1[band][ch];
                    stage->x1[band][ch] = in;
                    stage->y2[band][ch] = stage->y1[band][ch];
                    stage->y1[band][ch] = out;
                }

                in = (float)out;
            }

            block->interleaved[idx] = in;
        }
    }

    return 0;
}
