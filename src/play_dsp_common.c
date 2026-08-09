#include "play_dsp_common.h"
#include "play_eq_dynamic.h"
#include "play_eq_linear.h"
#include "play_eq_normal.h"
#include "play_pipeline.h"
#include "play_pipeline_eq_stages.h"
#include "play_multiband_dynamics.h"

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#include <math.h>
#include <string.h>

#define LN_1000_F 6.90775527898f

#ifdef USE_CMSIS_DSP
static float cmsis_scalar_exp(float x)
{
    float in = x;
    float out = 0.0f;
    arm_vexp_f32(&in, &out, 1);
    return out;
}
#endif

static void spectrum_init_bins(play_dsp_state* state)
{
    for (int i = 0; i < ANALYZER_BINS; ++i)
    {
        float t = (float)i / (float)(ANALYZER_BINS - 1);
        float freq;
        int k;

#ifdef USE_CMSIS_DSP
        freq = 20.0f * cmsis_scalar_exp(LN_1000_F * t);
#else
        freq = 20.0f * powf(1000.0f, t);
#endif

        k = (int)((freq * (float)ANALYZER_WINDOW / (float)state->sampleRate) + 0.5f);
        if (k < 1)
        {
            k = 1;
        }
        if (k > (ANALYZER_WINDOW / 2 - 1))
        {
            k = ANALYZER_WINDOW / 2 - 1;
        }
        state->binIndex[i] = k;
    }
}

static void update_low_crossfeed_coeff(play_dsp_state* state)
{
    float w = 2.0f * (float)M_PI * PLAY_LOW_CROSSFEED_CUTOFF_HZ / (float)state->sampleRate;
    state->lowCrossfeedLpA = expf(-w);
}

static void rebuild_eq(play_dsp_state* state)
{
    play_eq_assign_band_modes(state);
    play_eq_linear_rebuild(state);
    play_eq_normal_rebuild(state);
}

static void eq_pipeline_copy_from_state(play_dsp_state* state,
                                        play_linear_eq_stage* linearStage,
                                        play_dynamic_eq_stage* dynamicStage,
                                        play_normal_eq_stage* normalStage)
{
    for (int i = 0; i < PLAY_EQ_LINEAR_MAX_TAPS; ++i)
    {
        linearStage->linear.coeff[i] = state->linearFirCoeff[i];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            linearStage->linear.hist[i][ch] = state->linearFirHist[i][ch];
        }
    }
    linearStage->linear.firEnabled = state->linearFirEnabled;
    linearStage->linear.userEnabled = state->linearFirUserEnabled;
    linearStage->linear.taps = state->linearFirTaps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        linearStage->linear.writeIndex[ch] = state->linearFirWriteIndex[ch];
        linearStage->linear.transientEnv[ch] = state->linearFirTransientEnv[ch];
        linearStage->linear.lastDelayed[ch] = state->linearFirLastDelayed[ch];
    }

    for (int band = 0; band < EQ_BANDS; ++band)
    {
        dynamicStage->dynamicReductionDB[band] = state->dynamicReductionDB[band];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            dynamicStage->dynamicEnv[band][ch] = state->dynamicEnv[band][ch];
            dynamicStage->svfIc1eq[band][ch] = state->svfIc1eq[band][ch];
            dynamicStage->svfIc2eq[band][ch] = state->svfIc2eq[band][ch];
            dynamicStage->x1[band][ch] = state->x1[band][ch];
            dynamicStage->x2[band][ch] = state->x2[band][ch];
            dynamicStage->y1[band][ch] = state->y1[band][ch];
            dynamicStage->y2[band][ch] = state->y2[band][ch];

            normalStage->svfIc1eq[band][ch] = state->svfIc1eq[band][ch];
            normalStage->svfIc2eq[band][ch] = state->svfIc2eq[band][ch];
            normalStage->x1[band][ch] = state->x1[band][ch];
            normalStage->x2[band][ch] = state->x2[band][ch];
            normalStage->y1[band][ch] = state->y1[band][ch];
            normalStage->y2[band][ch] = state->y2[band][ch];
        }
    }

    play_dynamic_eq_stage_set_params(dynamicStage,
                                     state->dynamicAttack,
                                     state->dynamicRelease,
                                     state->dynamicThreshold,
                                     state->dynamicMaxReductionDB,
                                     state->dynamicStrengthDB);
}

static void eq_pipeline_copy_to_state(play_dsp_state* state,
                                      const play_linear_eq_stage* linearStage,
                                      const play_dynamic_eq_stage* dynamicStage,
                                      const play_normal_eq_stage* normalStage)
{
    for (int i = 0; i < PLAY_EQ_LINEAR_MAX_TAPS; ++i)
    {
        state->linearFirCoeff[i] = linearStage->linear.coeff[i];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->linearFirHist[i][ch] = linearStage->linear.hist[i][ch];
        }
    }
    state->linearFirEnabled = linearStage->linear.firEnabled;
    state->linearFirUserEnabled = linearStage->linear.userEnabled;
    state->linearFirTaps = linearStage->linear.taps;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        state->linearFirWriteIndex[ch] = linearStage->linear.writeIndex[ch];
        state->linearFirTransientEnv[ch] = linearStage->linear.transientEnv[ch];
        state->linearFirLastDelayed[ch] = linearStage->linear.lastDelayed[ch];
    }

    for (int band = 0; band < EQ_BANDS; ++band)
    {
        state->dynamicReductionDB[band] = dynamicStage->dynamicReductionDB[band];
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->dynamicEnv[band][ch] = dynamicStage->dynamicEnv[band][ch];
        }

        if (state->bandModes[band] == (uint8_t)PLAY_EQ_MODE_DYNAMIC)
        {
            for (int ch = 0; ch < MAX_CHANNELS; ++ch)
            {
                state->svfIc1eq[band][ch] = dynamicStage->svfIc1eq[band][ch];
                state->svfIc2eq[band][ch] = dynamicStage->svfIc2eq[band][ch];
                state->x1[band][ch] = dynamicStage->x1[band][ch];
                state->x2[band][ch] = dynamicStage->x2[band][ch];
                state->y1[band][ch] = dynamicStage->y1[band][ch];
                state->y2[band][ch] = dynamicStage->y2[band][ch];
            }
        }
        else if (state->bandModes[band] == (uint8_t)PLAY_EQ_MODE_NORMAL)
        {
            for (int ch = 0; ch < MAX_CHANNELS; ++ch)
            {
                state->svfIc1eq[band][ch] = normalStage->svfIc1eq[band][ch];
                state->svfIc2eq[band][ch] = normalStage->svfIc2eq[band][ch];
                state->x1[band][ch] = normalStage->x1[band][ch];
                state->x2[band][ch] = normalStage->x2[band][ch];
                state->y1[band][ch] = normalStage->y1[band][ch];
                state->y2[band][ch] = normalStage->y2[band][ch];
            }
        }
    }
}

static float goertzel_magnitude(const float* samples, int sampleCount, int k)
{
    float w;
    float coeff;
    float q0;
    float q1;
    float q2;
    int i;

    w = (2.0f * (float)M_PI * (float)k) / (float)sampleCount;
    coeff = 2.0f * cosf(w);
    q0 = 0.0f;
    q1 = 0.0f;
    q2 = 0.0f;

    for (i = 0; i < sampleCount; ++i)
    {
        q0 = coeff * q1 - q2 + samples[i];
        q2 = q1;
        q1 = q0;
    }

    return sqrtf(q1 * q1 + q2 * q2 - q1 * q2 * coeff);
}

static float goertzel_phase(const float* samples, int sampleCount, int k)
{
    float w = (2.0f * (float)M_PI * (float)k) / (float)sampleCount;
    float coeff = 2.0f * cosf(w);
    float q0 = 0.0f;
    float q1 = 0.0f;
    float q2 = 0.0f;
    float real;
    float imag;

    for (int i = 0; i < sampleCount; ++i)
    {
        q0 = coeff * q1 - q2 + samples[i];
        q2 = q1;
        q1 = q0;
    }

    real = q1 - q2 * cosf(w);
    imag = q2 * sinf(w);
    return atan2f(imag, real);
}

static float wrap_pi(float x)
{
    while (x > (float)M_PI)
    {
        x -= 2.0f * (float)M_PI;
    }
    while (x < -(float)M_PI)
    {
        x += 2.0f * (float)M_PI;
    }
    return x;
}

static void update_phase_metrics(play_dsp_state* state)
{
    float phaseDiff[ANALYZER_BINS];

    for (int i = 0; i < ANALYZER_BINS; ++i)
    {
        int k = state->binIndex[i];
        float pPre = goertzel_phase(state->preSamples, ANALYZER_WINDOW, k);
        float pPost = goertzel_phase(state->postSamples, ANALYZER_WINDOW, k);
        float d = wrap_pi(pPost - pPre);

        if (i > 0)
        {
            float prev = phaseDiff[i - 1];
            while (d - prev > (float)M_PI)
            {
                d -= 2.0f * (float)M_PI;
            }
            while (d - prev < -(float)M_PI)
            {
                d += 2.0f * (float)M_PI;
            }
        }

        phaseDiff[i] = d;
        state->phaseDeltaDeg[i] = d * (180.0f / (float)M_PI);
    }

    for (int i = 0; i < ANALYZER_BINS; ++i)
    {
        int i0 = (i == 0) ? 0 : i - 1;
        int i1 = (i == ANALYZER_BINS - 1) ? ANALYZER_BINS - 1 : i + 1;
        float f0 = (float)state->binIndex[i0] * (float)state->sampleRate / (float)ANALYZER_WINDOW;
        float f1 = (float)state->binIndex[i1] * (float)state->sampleRate / (float)ANALYZER_WINDOW;
        float dp = phaseDiff[i1] - phaseDiff[i0];
        float df = f1 - f0;
        float gd = 0.0f;

        if (df > 1e-6f)
        {
            gd = -dp / (2.0f * (float)M_PI * df);
        }

        state->groupDelayMs[i] = gd * 1000.0f;
    }
}

static void analyze_bins_for_samples(play_dsp_state* state, const float* samples, float* outBins)
{
#ifdef USE_CMSIS_DSP
    if (state->rfftReady)
    {
        for (int i = 0; i < ANALYZER_WINDOW; ++i)
        {
            state->fftIn[i] = samples[i];
        }

        arm_rfft_fast_f32(&state->rfft, state->fftIn, state->fftOut, 0);
        arm_cmplx_mag_f32(state->fftOut, state->fftMag, ANALYZER_WINDOW / 2);

        for (int i = 0; i < ANALYZER_WINDOW / 2; ++i)
        {
            state->fftMag[i] += 1e-8f;
        }

        arm_vlog_f32(state->fftMag, state->fftLog, ANALYZER_WINDOW / 2);

        for (int i = 0; i < ANALYZER_BINS; ++i)
        {
            int k = state->binIndex[i];
            float db = 8.6858896f * state->fftLog[k];
            if (!isfinite(db))
            {
                db = -80.0f;
            }
            outBins[i] = outBins[i] * 0.72f + db * 0.28f;
        }
        return;
    }
#endif

    for (int i = 0; i < ANALYZER_BINS; ++i)
    {
        int k = state->binIndex[i];
        float mag = goertzel_magnitude(samples, ANALYZER_WINDOW, k) / (float)ANALYZER_WINDOW;
        float db = 20.0f * log10f(mag + 1e-8f);
        if (!isfinite(db))
        {
            db = -80.0f;
        }
        outBins[i] = outBins[i] * 0.72f + db * 0.28f;
    }
}

static void update_spectrum(play_dsp_state* state)
{
    analyze_bins_for_samples(state, state->preSamples, state->preBins);
    analyze_bins_for_samples(state, state->postSamples, state->postBins);

    if ((state->pluginMask & PLAY_DSP_PLUGIN_PHASE_ANALYZER) != 0u)
    {
        update_phase_metrics(state);
    }
}

static void observer_window_complete(play_dsp_state* state)
{
    update_spectrum(state);
}

int play_dsp_init(play_dsp_state* state, uint32_t sampleRate, uint32_t channels)
{
    memset(state, 0, sizeof(*state));

    state->sampleRate = sampleRate;
    state->channels = channels;

    play_eq_normal_init_default_profile(state);
    play_eq_dynamic_init_params_state(state);
    state->bypassEnabled = 0u;
    state->lowCrossfeedEnabled = 0u;
    state->lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    play_eq_linear_init(state);
    play_mb_dyn_init(state);
    update_low_crossfeed_coeff(state);

    spectrum_init_bins(state);
    rebuild_eq(state);

#ifdef USE_CMSIS_DSP
    if (arm_rfft_fast_init_f32(&state->rfft, ANALYZER_WINDOW) != ARM_MATH_SUCCESS)
    {
        state->rfftReady = 0;
        return -1;
    }

    state->rfftReady = 1;
#endif

    return 0;
}

void play_dsp_set_eq_params(play_dsp_state* state, const float* gains, int gainCount, const float* qs, int qCount)
{
    int gn = (gainCount < EQ_BANDS) ? gainCount : EQ_BANDS;
    int qn = (qCount < EQ_BANDS) ? qCount : EQ_BANDS;

    for (int i = 0; i < gn; ++i)
    {
        float g = gains[i];
        if (g < EQ_MIN_GAIN_DB)
        {
            g = EQ_MIN_GAIN_DB;
        }
        if (g > EQ_MAX_GAIN_DB)
        {
            g = EQ_MAX_GAIN_DB;
        }
        state->gainsDB[i] = g;
    }

    for (int i = 0; i < qn; ++i)
    {
        float q = qs[i];
        if (q < EQ_MIN_Q)
        {
            q = EQ_MIN_Q;
        }
        if (q > EQ_MAX_Q)
        {
            q = EQ_MAX_Q;
        }
        state->qValues[i] = q;
    }

    rebuild_eq(state);
}

void play_dsp_set_dynamic_params(play_dsp_state* state,
                                 float attack,
                                 float release,
                                 float threshold,
                                 float maxReductionDB,
                                 float strengthDB)
{
    play_eq_dynamic_set_params_state(state, attack, release, threshold, maxReductionDB, strengthDB);
}

void play_dsp_set_bypass(play_dsp_state* state, int enabled)
{
    state->bypassEnabled = enabled ? 1u : 0u;
}

int play_dsp_get_bypass(const play_dsp_state* state)
{
    return state->bypassEnabled ? 1 : 0;
}

void play_dsp_set_low_crossfeed(play_dsp_state* state, int enabled, uint8_t position)
{
    uint8_t pos = (position == (uint8_t)PLAY_CROSSFEED_POST_EQ)
                      ? (uint8_t)PLAY_CROSSFEED_POST_EQ
                      : (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    state->lowCrossfeedEnabled = enabled ? 1u : 0u;
    state->lowCrossfeedPosition = pos;
}

void play_dsp_get_low_crossfeed(const play_dsp_state* state, int* outEnabled, uint8_t* outPosition)
{
    if (outEnabled != NULL)
    {
        *outEnabled = state->lowCrossfeedEnabled ? 1 : 0;
    }
    if (outPosition != NULL)
    {
        *outPosition = state->lowCrossfeedPosition;
    }
}

void play_dsp_set_linear_fir_config(play_dsp_state* state, int enabled, int taps)
{
    play_eq_linear_set_config(state, enabled, taps);
    rebuild_eq(state);
}

void play_dsp_get_linear_fir_config(const play_dsp_state* state,
                                    int* outEnabled,
                                    int* outTaps,
                                    int* outDelaySamples,
                                    float* outDelayMs)
{
    play_eq_linear_get_config(state, outEnabled, outTaps, outDelaySamples, outDelayMs);
}

void play_dsp_set_multiband_dynamics_config(play_dsp_state* state,
                                            int enabled,
                                            uint8_t position,
                                            float threshold,
                                            float attack,
                                            float release,
                                            float strength)
{
    play_mb_dyn_set_config(state, enabled, position, threshold, attack, release, strength);
}

void play_dsp_get_multiband_dynamics_config(const play_dsp_state* state,
                                            int* outEnabled,
                                            uint8_t* outPosition,
                                            float* outThreshold,
                                            float* outAttack,
                                            float* outRelease,
                                            float* outStrength)
{
    play_mb_dyn_get_config(state, outEnabled, outPosition, outThreshold, outAttack, outRelease, outStrength);
}

void play_dsp_set_multiband_dynamics_amounts(play_dsp_state* state, const float* amountsDb, int count)
{
    play_mb_dyn_set_amounts(state, amountsDb, count);
}

void play_dsp_copy_multiband_dynamics_amounts(const play_dsp_state* state, float* outAmountsDb, int maxCount)
{
    play_mb_dyn_copy_amounts(state, outAmountsDb, maxCount);
}

void play_dsp_copy_multiband_dynamics_applied_db(const play_dsp_state* state, float* outAppliedDb, int maxCount)
{
    play_mb_dyn_copy_applied_db(state, outAppliedDb, maxCount);
}

void play_dsp_copy_multiband_dynamics_bands(const play_dsp_state* state,
                                            float* outBandLowHz,
                                            float* outBandHighHz,
                                            int maxCount)
{
    play_mb_dyn_copy_bands(state, outBandLowHz, outBandHighHz, maxCount);
}

void play_dsp_set_plugin_enabled(play_dsp_state* state, uint32_t pluginBit, int enabled)
{
    if (enabled)
    {
        state->pluginMask |= pluginBit;
    }
    else
    {
        state->pluginMask &= ~pluginBit;
    }
}

uint32_t play_dsp_get_plugin_mask(const play_dsp_state* state)
{
    return state->pluginMask;
}

void play_dsp_get_dynamic_params(const play_dsp_state* state,
                                 float* outAttack,
                                 float* outRelease,
                                 float* outThreshold,
                                 float* outMaxReductionDB,
                                 float* outStrengthDB)
{
    play_eq_dynamic_get_params_state(state, outAttack, outRelease, outThreshold, outMaxReductionDB, outStrengthDB);
}

void play_dsp_process(play_dsp_state* state, float* interleavedFrames, uint32_t frameCount, uint32_t channels)
{
    play_pipeline prePipeline;
    play_pipeline eqPipeline;
    play_pipeline postPipeline;
    play_observer_metrics observerMetrics;
    play_observer_stage observerRawInStage;
    play_observer_stage observerEqInStage;
    play_observer_stage observerEqOutStage;
    play_observer_stage observerOutStage;
    play_eq_stage_profile profile;
    play_crossfeed_stage preCrossfeedStage;
    play_mbdyn_stage preMbDynStage;
    play_linear_eq_stage linearStage;
    play_dynamic_eq_stage dynamicStage;
    play_normal_eq_stage normalStage;
    play_mbdyn_stage postMbDynStage;
    play_crossfeed_stage postCrossfeedStage;

    play_eq_stage_profile_init(&profile,
                               state->sampleRate,
                               channels,
                               state->bandFreqs,
                               state->gainsDB,
                               state->qValues,
                               state->bandModes,
                               EQ_BANDS);

    play_linear_eq_stage_init(&linearStage,
                              &profile,
                              state->linearFirUserEnabled ? 1 : 0,
                              (int)state->linearFirTaps,
                              (uint8_t)PLAY_EQ_MODE_LINEAR);
    play_dynamic_eq_stage_init(&dynamicStage,
                               state->sampleRate,
                               channels,
                               state->bandFreqs,
                               state->gainsDB,
                               state->qValues,
                               state->bandModes,
                               EQ_BANDS,
                               (uint8_t)PLAY_EQ_MODE_LINEAR,
                               (uint8_t)PLAY_EQ_MODE_DYNAMIC);
    play_normal_eq_stage_init(&normalStage,
                              state->sampleRate,
                              channels,
                              state->bandFreqs,
                              state->gainsDB,
                              state->qValues,
                              state->bandModes,
                              EQ_BANDS,
                              (uint8_t)PLAY_EQ_MODE_LINEAR,
                              (uint8_t)PLAY_EQ_MODE_NORMAL);

    play_crossfeed_stage_init(&preCrossfeedStage, state, (uint8_t)PLAY_CROSSFEED_PRE_EQ);
    play_mbdyn_stage_init(&preMbDynStage, state, (uint8_t)PLAY_CROSSFEED_PRE_EQ);
    play_mbdyn_stage_init(&postMbDynStage, state, (uint8_t)PLAY_CROSSFEED_POST_EQ);
    play_crossfeed_stage_init(&postCrossfeedStage, state, (uint8_t)PLAY_CROSSFEED_POST_EQ);
    play_observer_stage_init(&observerRawInStage,
                             state,
                             &observerMetrics,
                             PLAY_OBSERVER_INPUT_RAW,
                             NULL);
    play_observer_stage_init(&observerEqInStage,
                             state,
                             &observerMetrics,
                             PLAY_OBSERVER_INPUT_EQ,
                             NULL);
    play_observer_stage_init(&observerEqOutStage,
                             state,
                             &observerMetrics,
                             PLAY_OBSERVER_OUTPUT_EQ,
                             NULL);
    play_observer_stage_init(&observerOutStage,
                             state,
                             &observerMetrics,
                             PLAY_OBSERVER_OUTPUT_FINAL,
                             observer_window_complete);

    eq_pipeline_copy_from_state(state, &linearStage, &dynamicStage, &normalStage);

    play_pipeline_init(&prePipeline);
    (void)play_pipeline_add_stage(&prePipeline, "observe-input", 1, &observerRawInStage, play_observer_stage_process);
    (void)play_pipeline_add_stage(&prePipeline,
                                  "pre-crossfeed",
                                  state->bypassEnabled ? 0 : 1,
                                  &preCrossfeedStage,
                                  play_crossfeed_stage_process);
    (void)play_pipeline_add_stage(&prePipeline,
                                  "pre-mbdyn",
                                  state->bypassEnabled ? 0 : 1,
                                  &preMbDynStage,
                                  play_mbdyn_stage_process);
    (void)play_pipeline_add_stage(&prePipeline, "observe-eq-in", 1, &observerEqInStage, play_observer_stage_process);

    play_pipeline_init(&eqPipeline);
    (void)play_pipeline_add_stage(&eqPipeline,
                                  "linear-eq",
                                  state->bypassEnabled ? 0 : 1,
                                  &linearStage,
                                  play_linear_eq_stage_process);
    (void)play_pipeline_add_stage(&eqPipeline,
                                  "dynamic-eq",
                                  state->bypassEnabled ? 0 : 1,
                                  &dynamicStage,
                                  play_dynamic_eq_stage_process);
    (void)play_pipeline_add_stage(&eqPipeline,
                                  "normal-eq",
                                  state->bypassEnabled ? 0 : 1,
                                  &normalStage,
                                  play_normal_eq_stage_process);
    (void)play_pipeline_add_stage(&eqPipeline, "observe-eq-out", 1, &observerEqOutStage, play_observer_stage_process);

    play_pipeline_init(&postPipeline);
    (void)play_pipeline_add_stage(&postPipeline,
                                  "post-mbdyn",
                                  state->bypassEnabled ? 0 : 1,
                                  &postMbDynStage,
                                  play_mbdyn_stage_process);
    (void)play_pipeline_add_stage(&postPipeline,
                                  "post-crossfeed",
                                  state->bypassEnabled ? 0 : 1,
                                  &postCrossfeedStage,
                                  play_crossfeed_stage_process);
    (void)play_pipeline_add_stage(&postPipeline, "observe-output", 1, &observerOutStage, play_observer_stage_process);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        float frameBuf[MAX_CHANNELS] = {0.0f, 0.0f};
        play_frame_block block;
        uint32_t pipelineChannels = (channels > MAX_CHANNELS) ? MAX_CHANNELS : channels;

        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            float inputSample = interleavedFrames[i * channels + ch];
            if (ch < MAX_CHANNELS)
            {
                frameBuf[ch] = inputSample;
            }
        }

        observerMetrics.monoIn = 0.0f;
        observerMetrics.monoEqIn = 0.0f;
        observerMetrics.monoEqOut = 0.0f;
        observerMetrics.monoOut = 0.0f;

        block.interleaved = frameBuf;
        block.frameCount = 1u;
        block.channels = pipelineChannels;
        block.sampleRate = state->sampleRate;
        (void)play_pipeline_run(&prePipeline, &block);

        (void)play_pipeline_run(&eqPipeline, &block);

        (void)play_pipeline_run(&postPipeline, &block);

        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            float outSample = (ch < MAX_CHANNELS) ? frameBuf[ch] : interleavedFrames[i * channels + ch];
            interleavedFrames[i * channels + ch] = outSample;
        }
    }

    eq_pipeline_copy_to_state(state, &linearStage, &dynamicStage, &normalStage);
}

void play_dsp_copy_bins(const play_dsp_state* state, float* outBins, int maxCount)
{
    int n = (maxCount < ANALYZER_BINS) ? maxCount : ANALYZER_BINS;
    for (int i = 0; i < n; ++i)
    {
        outBins[i] = state->postBins[i];
    }
}

void play_dsp_copy_spectrum(const play_dsp_state* state, float* outPreBins, float* outPostBins, int maxCount)
{
    int n = (maxCount < ANALYZER_BINS) ? maxCount : ANALYZER_BINS;
    for (int i = 0; i < n; ++i)
    {
        if (outPreBins != NULL)
        {
            outPreBins[i] = state->preBins[i];
        }
        if (outPostBins != NULL)
        {
            outPostBins[i] = state->postBins[i];
        }
    }
}

void play_dsp_copy_eq_taps(const play_dsp_state* state,
                           float* outEqIn,
                           float* outEqOut,
                           int maxCount,
                           int* outWriteIndex,
                           uint64_t* outSeq)
{
    int n = (maxCount < ANALYZER_WINDOW) ? maxCount : ANALYZER_WINDOW;

    for (int i = 0; i < n; ++i)
    {
        if (outEqIn != NULL)
        {
            outEqIn[i] = state->eqTapInSamples[i];
        }
        if (outEqOut != NULL)
        {
            outEqOut[i] = state->eqTapOutSamples[i];
        }
    }

    if (outWriteIndex != NULL)
    {
        *outWriteIndex = state->writeIndex;
    }
    if (outSeq != NULL)
    {
        *outSeq = state->eqTapSeq;
    }
}

void play_dsp_copy_dynamic_curve(const play_dsp_state* state, uint8_t* outModes, float* outReductionDB, int maxCount)
{
    int n = (maxCount < EQ_BANDS) ? maxCount : EQ_BANDS;
    for (int i = 0; i < n; ++i)
    {
        if (outModes != NULL)
        {
            outModes[i] = state->bandModes[i];
        }
        if (outReductionDB != NULL)
        {
            outReductionDB[i] = state->dynamicReductionDB[i];
        }
    }
}

void play_dsp_copy_phase_metrics(const play_dsp_state* state, float* outPhaseDeltaDeg, float* outGroupDelayMs,
                                 int maxCount)
{
    int n = (maxCount < ANALYZER_BINS) ? maxCount : ANALYZER_BINS;
    for (int i = 0; i < n; ++i)
    {
        if (outPhaseDeltaDeg != NULL)
        {
            outPhaseDeltaDeg[i] = state->phaseDeltaDeg[i];
        }
        if (outGroupDelayMs != NULL)
        {
            outGroupDelayMs[i] = state->groupDelayMs[i];
        }
    }
}

void play_dsp_copy_eq(const play_dsp_state* state, float* outFreqs, float* outGains, float* outQs, int maxCount)
{
    int n = (maxCount < EQ_BANDS) ? maxCount : EQ_BANDS;
    for (int i = 0; i < n; ++i)
    {
        outFreqs[i] = state->bandFreqs[i];
        outGains[i] = state->gainsDB[i];
        outQs[i] = state->qValues[i];
    }
}
