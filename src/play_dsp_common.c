#include "play_dsp_common.h"

#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#include <math.h>
#include <string.h>

#define LN_1000_F 6.90775527898f
#define DYNAMIC_EQ_DEFAULT_ATTACK 0.12f
#define DYNAMIC_EQ_DEFAULT_RELEASE 0.02f
#define DYNAMIC_EQ_DEFAULT_THRESHOLD 0.18f
#define DYNAMIC_EQ_DEFAULT_MAX_REDUCTION_DB 12.0f
#define DYNAMIC_EQ_DEFAULT_STRENGTH_DB 18.0f

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

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return v;
}

static void update_low_crossfeed_coeff(play_dsp_state* state)
{
    float w = 2.0f * (float)M_PI * PLAY_LOW_CROSSFEED_CUTOFF_HZ / (float)state->sampleRate;
    state->lowCrossfeedLpA = expf(-w);
}

static float low_crossfeed_lp_process(play_dsp_state* state, uint32_t ch, float x)
{
    float y = (1.0f - state->lowCrossfeedLpA) * x + state->lowCrossfeedLpA * state->lowCrossfeedLpState[ch];
    state->lowCrossfeedLpState[ch] = y;
    return y;
}

static void assign_band_modes(play_dsp_state* state)
{
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        float freq = state->bandFreqs[i];
        if (freq <= EQ_LINEAR_MAX_HZ)
        {
            state->bandModes[i] = (uint8_t)PLAY_EQ_MODE_LINEAR;
        }
        else if (freq <= EQ_DYNAMIC_MAX_HZ)
        {
            state->bandModes[i] = (uint8_t)PLAY_EQ_MODE_DYNAMIC;
        }
        else
        {
            state->bandModes[i] = (uint8_t)PLAY_EQ_MODE_NORMAL;
        }
    }
}

static void rebuild_eq(play_dsp_state* state)
{
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        double gainDB = state->gainsDB[i];
        double q = state->qValues[i];
        double frequency = state->bandFreqs[i];
        uint8_t mode = state->bandModes[i];
        double a;
        double w0;
        double cosw0;
        double sinw0;
        double alpha;
        double b0;
        double b1;
        double b2;
        double a0;
        double a1;
        double a2;

        if (mode == (uint8_t)PLAY_EQ_MODE_LINEAR)
        {
            float linearAmplitude = 1.0f + (float)(gainDB / (double)EQ_MAX_GAIN_DB);
            if (linearAmplitude < 0.05f)
            {
                linearAmplitude = 0.05f;
            }
            gainDB = 20.0 * log10((double)linearAmplitude);
        }

        state->bandUseSVF[i] = (frequency <= (double)EQ_LOW_SVF_MAX_HZ) ? 1u : 0u;

        if (state->bandUseSVF[i])
        {
            double g = tan(M_PI * frequency / (double)state->sampleRate);
            double k = 1.0 / q;
            double aBell = pow(10.0, gainDB / 40.0);
            double h = 1.0 / (1.0 + g * (g + k));

            state->svfG[i] = g;
            state->svfK[i] = k;
            state->svfA[i] = aBell;
            state->svfH[i] = h;
            continue;
        }

        a = pow(10.0, gainDB / 40.0);
        w0 = 2.0 * M_PI * frequency / (double)state->sampleRate;
#ifdef USE_CMSIS_DSP
        cosw0 = arm_cos_f32((float32_t)w0);
        sinw0 = arm_sin_f32((float32_t)w0);
#else
        cosw0 = cos(w0);
        sinw0 = sin(w0);
#endif
        alpha = sinw0 / (2.0 * q);

        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cosw0;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a1 = -2.0 * cosw0;
        a2 = 1.0 - alpha / a;

        state->b0[i] = b0 / a0;
        state->b1[i] = b1 / a0;
        state->b2[i] = b2 / a0;
        state->a1[i] = a1 / a0;
        state->a2[i] = a2 / a0;
    }
}

static float process_tpt_svf_band(play_dsp_state* state, int band, uint32_t ch, float in)
{
    double g = state->svfG[band];
    double k = state->svfK[band];
    double aBell = state->svfA[band];
    double h = state->svfH[band];
    double ic1eq = state->svfIc1eq[band][ch];
    double ic2eq = state->svfIc2eq[band][ch];
    double v1;
    double v2;
    double bp;
    double out;

    v1 = h * (ic1eq + g * ((double)in - ic2eq));
    v2 = ic2eq + g * v1;
    bp = v1;
    out = (double)in + k * (aBell - 1.0) * bp;

    state->svfIc1eq[band][ch] = 2.0 * v1 - ic1eq;
    state->svfIc2eq[band][ch] = 2.0 * v2 - ic2eq;

    return (float)out;
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

int play_dsp_init(play_dsp_state* state, uint32_t sampleRate, uint32_t channels)
{
    memset(state, 0, sizeof(*state));

    state->sampleRate = sampleRate;
    state->channels = channels;

    {
        const float freqs[EQ_BANDS] = {31.0f, 62.0f, 125.0f, 250.0f, 500.0f, 2000.0f, 8000.0f, 18000.0f};
        const float gains[EQ_BANDS] = {3.0f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        const float qs[EQ_BANDS] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};

        for (int i = 0; i < EQ_BANDS; ++i)
        {
            state->bandFreqs[i] = freqs[i];
            state->gainsDB[i] = gains[i];
            state->qValues[i] = qs[i];
        }
    }

    state->dynamicAttack = DYNAMIC_EQ_DEFAULT_ATTACK;
    state->dynamicRelease = DYNAMIC_EQ_DEFAULT_RELEASE;
    state->dynamicThreshold = DYNAMIC_EQ_DEFAULT_THRESHOLD;
    state->dynamicMaxReductionDB = DYNAMIC_EQ_DEFAULT_MAX_REDUCTION_DB;
    state->dynamicStrengthDB = DYNAMIC_EQ_DEFAULT_STRENGTH_DB;
    state->bypassEnabled = 0u;
    state->lowCrossfeedEnabled = 0u;
    state->lowCrossfeedPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    update_low_crossfeed_coeff(state);

    assign_band_modes(state);

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

    assign_band_modes(state);
    rebuild_eq(state);
}

void play_dsp_set_dynamic_params(play_dsp_state* state,
                                 float attack,
                                 float release,
                                 float threshold,
                                 float maxReductionDB,
                                 float strengthDB)
{
    state->dynamicAttack = clampf(attack, DYNAMIC_EQ_MIN_ATTACK, DYNAMIC_EQ_MAX_ATTACK);
    state->dynamicRelease = clampf(release, DYNAMIC_EQ_MIN_RELEASE, DYNAMIC_EQ_MAX_RELEASE);
    state->dynamicThreshold = clampf(threshold, DYNAMIC_EQ_MIN_THRESHOLD, DYNAMIC_EQ_MAX_THRESHOLD);
    state->dynamicMaxReductionDB = clampf(maxReductionDB, DYNAMIC_EQ_MIN_MAX_REDUCTION_DB,
                                          DYNAMIC_EQ_MAX_MAX_REDUCTION_DB);
    state->dynamicStrengthDB = clampf(strengthDB, DYNAMIC_EQ_MIN_STRENGTH_DB, DYNAMIC_EQ_MAX_STRENGTH_DB);
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
    if (outAttack != NULL)
    {
        *outAttack = state->dynamicAttack;
    }
    if (outRelease != NULL)
    {
        *outRelease = state->dynamicRelease;
    }
    if (outThreshold != NULL)
    {
        *outThreshold = state->dynamicThreshold;
    }
    if (outMaxReductionDB != NULL)
    {
        *outMaxReductionDB = state->dynamicMaxReductionDB;
    }
    if (outStrengthDB != NULL)
    {
        *outStrengthDB = state->dynamicStrengthDB;
    }
}

void play_dsp_process(play_dsp_state* state, float* interleavedFrames, uint32_t frameCount, uint32_t channels)
{
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        float monoIn = 0.0f;
        float monoOut = 0.0f;
        float preCrossL = 0.0f;
        float preCrossR = 0.0f;
        float postCrossL = 0.0f;
        float postCrossR = 0.0f;

        if (!state->bypassEnabled && state->lowCrossfeedEnabled && channels >= 2u
            && state->lowCrossfeedPosition == (uint8_t)PLAY_CROSSFEED_PRE_EQ)
        {
            float inL = interleavedFrames[i * channels + 0];
            float inR = interleavedFrames[i * channels + 1];
            float lowL = low_crossfeed_lp_process(state, 0, inL);
            float lowR = low_crossfeed_lp_process(state, 1, inR);
            preCrossL = inL + PLAY_LOW_CROSSFEED_RATIO * lowR;
            preCrossR = inR + PLAY_LOW_CROSSFEED_RATIO * lowL;
        }

        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            float inputSample = interleavedFrames[i * channels + ch];
            float in = inputSample;

            monoIn += inputSample;

            if (!state->bypassEnabled && state->lowCrossfeedEnabled && channels >= 2u
                && state->lowCrossfeedPosition == (uint8_t)PLAY_CROSSFEED_PRE_EQ)
            {
                if (ch == 0u)
                {
                    in = preCrossL;
                }
                else if (ch == 1u)
                {
                    in = preCrossR;
                }
            }

            if (!state->bypassEnabled && ch < MAX_CHANNELS)
            {
                for (int band = 0; band < EQ_BANDS; ++band)
                {
                    double out;

                    if (state->bandUseSVF[band])
                    {
                        out = process_tpt_svf_band(state, band, ch, in);
                    }
                    else
                    {
                        out = state->b0[band] * in + state->b1[band] * state->x1[band][ch] + state->b2[band] * state->x2
                            [band][ch]
                            - state->a1[band] * state->y1[band][ch] - state->a2[band] * state->y2[band][ch];

                        state->x2[band][ch] = state->x1[band][ch];
                        state->x1[band][ch] = in;
                        state->y2[band][ch] = state->y1[band][ch];
                        state->y1[band][ch] = out;
                    }

                    if (state->bandModes[band] == (uint8_t)PLAY_EQ_MODE_DYNAMIC)
                    {
                        float env = state->dynamicEnv[band][ch];
                        float absOut = fabsf((float)out);
                        float coeff = (absOut > env) ? state->dynamicAttack : state->dynamicRelease;
                        float reductionDb = 0.0f;

                        env += coeff * (absOut - env);
                        state->dynamicEnv[band][ch] = env;

                        if (state->gainsDB[band] > 0.0f && env > state->dynamicThreshold)
                        {
                            float over = (env - state->dynamicThreshold);
                            reductionDb = over * state->dynamicStrengthDB;
                            if (reductionDb > state->dynamicMaxReductionDB)
                            {
                                reductionDb = state->dynamicMaxReductionDB;
                            }
                            out *= db_to_linear(-reductionDb);
                        }

                        state->dynamicReductionDB[band] = state->dynamicReductionDB[band] * 0.88f + reductionDb * 0.12f;
                    }

                    in = (float)out;
                }
            }

            if (!state->bypassEnabled && state->lowCrossfeedEnabled && channels >= 2u
                && state->lowCrossfeedPosition == (uint8_t)PLAY_CROSSFEED_POST_EQ)
            {
                if (ch == 0u)
                {
                    float low = low_crossfeed_lp_process(state, 0, in);
                    postCrossL = in;
                    postCrossR = PLAY_LOW_CROSSFEED_RATIO * low;
                }
                else if (ch == 1u)
                {
                    float low = low_crossfeed_lp_process(state, 1, in);
                    in += postCrossR;
                    postCrossL += PLAY_LOW_CROSSFEED_RATIO * low;
                }
            }

            interleavedFrames[i * channels + ch] = in;
            monoOut += in;
        }

        if (!state->bypassEnabled && state->lowCrossfeedEnabled && channels >= 2u
            && state->lowCrossfeedPosition == (uint8_t)PLAY_CROSSFEED_POST_EQ)
        {
            interleavedFrames[i * channels + 0] = postCrossL;
        }

        monoIn /= (float)channels;
        monoOut /= (float)channels;
        state->preSamples[state->writeIndex] = monoIn;
        state->postSamples[state->writeIndex] = monoOut;
        state->writeIndex++;

        if (state->writeIndex >= ANALYZER_WINDOW)
        {
            state->writeIndex = 0;
            update_spectrum(state);
        }
    }
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
