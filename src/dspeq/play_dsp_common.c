#include "play_dsp_common.h"
#include "play_eq_dynamic.h"
#include "play_eq_linear.h"
#include "play_eq_normal.h"
#include "eq_low_seq.h"
#include "play_eq_runtime.h"
#include "play_dsp_plugins.h"
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
    float w = 2.0f * (float)PLAY_DSP_PI * PLAY_LOW_CROSSFEED_CUTOFF_HZ / (float)state->sampleRate;
    state->lowCrossfeedLpA = expf(-w);
}

static void rebuild_eq(play_dsp_state* state)
{
    play_eq_assign_band_modes(state);
    play_eq_linear_rebuild(state);
    play_eq_normal_rebuild(state);
}

static float goertzel_magnitude(const float* samples, int sampleCount, int k)
{
    float w;
    float coeff;
    float q0;
    float q1;
    float q2;
    int i;

    w = (2.0f * (float)PLAY_DSP_PI * (float)k) / (float)sampleCount;
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
    float w = (2.0f * (float)PLAY_DSP_PI * (float)k) / (float)sampleCount;
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
    while (x > (float)PLAY_DSP_PI)
    {
        x -= 2.0f * (float)PLAY_DSP_PI;
    }
    while (x < -(float)PLAY_DSP_PI)
    {
        x += 2.0f * (float)PLAY_DSP_PI;
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
            while (d - prev > (float)PLAY_DSP_PI)
            {
                d -= 2.0f * (float)PLAY_DSP_PI;
            }
            while (d - prev < -(float)PLAY_DSP_PI)
            {
                d += 2.0f * (float)PLAY_DSP_PI;
            }
        }

        phaseDiff[i] = d;
        state->phaseDeltaDeg[i] = d * (180.0f / (float)PLAY_DSP_PI);
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
            gd = -dp / (2.0f * (float)PLAY_DSP_PI * df);
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
    static const float kLowSeqDefaultFreqs[5] = {35.0f, 60.0f, 110.0f, 220.0f, 300.0f};
    static const float kLowSeqDefaultGains[5] = {6.0f, 6.0f, 4.0f, 2.5f, 1.5f};
    static const float kLowSeqDefaultQs[5] = {2.2f, 2.1f, 1.8f, 1.4f, 1.2f};

    memset(state, 0, sizeof(*state));

    state->sampleRate = sampleRate;
    state->channels = channels;
    state->activeEqBands = EQ_BANDS;
    state->activeEqBands = EQ_BANDS;

    play_eq_normal_init_default_profile(state);
    play_eq_dynamic_init_params_state(state);
    state->bypassEnabled = 0u;
    state->lowCrossfeedEnabled = 0u;
    state->lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    play_eq_linear_init(state);
    play_mb_dyn_init(state);
    eq_low_seq_init(&state->lowSeq, sampleRate, channels);
    eq_low_seq_set_mode(&state->lowSeq, EQ_LOW_SEQ_MODE_BALANCED);
    eq_low_seq_set_profile(&state->lowSeq,
                           kLowSeqDefaultFreqs,
                           kLowSeqDefaultGains,
                           kLowSeqDefaultQs,
                           5,
                           1);
    state->lowSeqEnabled = 1u;
    state->lowSeqMode = (uint8_t)EQ_LOW_SEQ_MODE_BALANCED;
    play_dsp_plugins_registry_init(state);
    state->timingBefore = NULL;
    state->timingAfter = NULL;
    state->timingHookUserData = NULL;
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

void play_dsp_set_low_seq_enabled(play_dsp_state* state, int enabled)
{
    if (state == NULL)
    {
        return;
    }

    state->lowSeqEnabled = enabled ? 1u : 0u;
}

int play_dsp_get_low_seq_enabled(const play_dsp_state* state)
{
    if (state == NULL)
    {
        return 0;
    }

    return state->lowSeqEnabled ? 1 : 0;
}

void play_dsp_set_low_seq_mode(play_dsp_state* state, eq_low_seq_mode mode)
{
    if (state == NULL)
    {
        return;
    }

    eq_low_seq_set_mode(&state->lowSeq, mode);
    state->lowSeqMode = (uint8_t)mode;
}

void play_dsp_set_low_seq_band(play_dsp_state* state, int bandIndex, float freqHz, float gainDb, float q, int enabled)
{
    if (state == NULL)
    {
        return;
    }

    eq_low_seq_set_band(&state->lowSeq, bandIndex, freqHz, gainDb, q, enabled);
}

void play_dsp_set_low_seq_profile(play_dsp_state* state,
                                  const float* freqsHz,
                                  const float* gainsDb,
                                  const float* qs,
                                  int count,
                                  int enabled)
{
    if (state == NULL)
    {
        return;
    }

    eq_low_seq_set_profile(&state->lowSeq, freqsHz, gainsDb, qs, count, enabled);
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

int play_dsp_plugin_set_order(play_dsp_state* state, const uint8_t* order, int count)
{
    return play_dsp_plugins_set_order(state, order, count);
}

int play_dsp_plugin_set_bypass(play_dsp_state* state, uint8_t pluginId, int bypass)
{
    return play_dsp_plugins_set_bypass(state, pluginId, bypass);
}

int play_dsp_plugin_set_channel_mask(play_dsp_state* state, uint8_t pluginId, uint8_t channelMask)
{
    return play_dsp_plugins_set_channel_mask(state, pluginId, channelMask);
}

int play_dsp_plugin_set_channel_bypass(play_dsp_state* state, uint32_t channel, uint8_t pluginId, int bypass)
{
    return play_dsp_plugins_set_channel_bypass(state, channel, pluginId, bypass);
}

int play_dsp_plugin_validate(play_dsp_state* state)
{
    return play_dsp_plugins_validate(state);
}

void play_dsp_set_timing_triggers(play_dsp_state* state,
                                  play_dsp_timing_trigger_fn before,
                                  play_dsp_timing_trigger_fn after,
                                  void* userData)
{
    if (state == NULL)
    {
        return;
    }

    state->timingBefore = before;
    state->timingAfter = after;
    state->timingHookUserData = userData;
}

void play_dsp_get_timing_status(const play_dsp_state* state,
                                uint64_t* outSequence,
                                double* outBudgetMs,
                                double* outProcessMs,
                                double* outOverrunMs,
                                int* outSourceThrottleHint)
{
    if (state == NULL)
    {
        return;
    }

    if (outSequence != NULL)
    {
        *outSequence = state->dspProcessSeq;
    }
    if (outBudgetMs != NULL)
    {
        *outBudgetMs = state->dspLastBudgetMs;
    }
    if (outProcessMs != NULL)
    {
        *outProcessMs = state->dspLastProcessMs;
    }
    if (outOverrunMs != NULL)
    {
        *outOverrunMs = state->dspLastOverrunMs;
    }
    if (outSourceThrottleHint != NULL)
    {
        *outSourceThrottleHint = state->sourceThrottleHint ? 1 : 0;
    }
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
    uint32_t pipelineChannels = (channels > MAX_CHANNELS) ? MAX_CHANNELS : channels;
    float planarStorage[MAX_CHANNELS][frameCount > 0u ? frameCount : 1u];
    float* planarPtrs[MAX_CHANNELS] = {0};

    if (state == NULL || interleavedFrames == NULL || frameCount == 0u || channels == 0u)
    {
        return;
    }

    for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
    {
        planarPtrs[ch] = planarStorage[ch];
    }

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            planarStorage[ch][i] = interleavedFrames[i * channels + ch];
        }
    }

    play_dsp_process_planar(state, planarPtrs, frameCount, pipelineChannels);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            interleavedFrames[i * channels + ch] = planarStorage[ch][i];
        }
    }
}

void play_dsp_process_planar(play_dsp_state* state,
                             float* const* planarFrames,
                             uint32_t frameCount,
                             uint32_t channels)
{
    play_observer_metrics observerMetrics;
    static play_dsp_plugins_runtime pluginsRuntime;
    play_frame_block block;
    uint32_t pipelineChannels;

    if (state == NULL || planarFrames == NULL || frameCount == 0u || channels == 0u)
    {
        return;
    }

    pipelineChannels = (channels > MAX_CHANNELS) ? MAX_CHANNELS : channels;

    observerMetrics.monoIn = 0.0f;
    observerMetrics.monoEqIn = 0.0f;
    observerMetrics.monoEqOut = 0.0f;
    observerMetrics.monoOut = 0.0f;

    block.interleaved = NULL;
    block.planar = planarFrames;
    block.layout = PLAY_BUFFER_LAYOUT_PLANAR;
    block.frameCount = frameCount;
    block.channels = pipelineChannels;
    block.channelMask = PLAY_DSP_CHANNEL_BOTH;
    block.sampleRate = state->sampleRate;

    play_dsp_plugins_timing_begin(state, frameCount, pipelineChannels);
    play_dsp_plugins_prepare(&pluginsRuntime,
                             state,
                             &block,
                             pipelineChannels,
                             &observerMetrics,
                             observer_window_complete);
    play_dsp_plugins_run(&pluginsRuntime);
    play_dsp_plugins_commit(&pluginsRuntime);
    play_dsp_plugins_timing_end(state, frameCount, pipelineChannels);
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
