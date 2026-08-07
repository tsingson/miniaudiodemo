#include "play_dsp_common.h"

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

static void rebuild_eq(play_dsp_state* state)
{
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        double gainDB = state->gainsDB[i];
        double q = state->qValues[i];
        double frequency = state->bandFreqs[i];
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

static void update_spectrum(play_dsp_state* state)
{
#ifdef USE_CMSIS_DSP
    if (state->rfftReady)
    {
        for (int i = 0; i < ANALYZER_WINDOW; ++i)
        {
            state->fftIn[i] = state->samples[i];
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
            state->bins[i] = state->bins[i] * 0.72f + db * 0.28f;
        }
        return;
    }
#endif

    for (int i = 0; i < ANALYZER_BINS; ++i)
    {
        int k = state->binIndex[i];
        float mag = goertzel_magnitude(state->samples, ANALYZER_WINDOW, k) / (float)ANALYZER_WINDOW;
        float db = 20.0f * log10f(mag + 1e-8f);
        if (!isfinite(db))
        {
            db = -80.0f;
        }
        state->bins[i] = state->bins[i] * 0.72f + db * 0.28f;
    }
}

int play_dsp_init(play_dsp_state* state, uint32_t sampleRate, uint32_t channels)
{
    memset(state, 0, sizeof(*state));

    state->sampleRate = sampleRate;
    state->channels = channels;

    {
        const float freqs[EQ_BANDS] = {31.0f, 62.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 3000.0f, 8000.0f};
        const float gains[EQ_BANDS] = {3.0f, 3.0f, 3.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        const float qs[EQ_BANDS] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};

        for (int i = 0; i < EQ_BANDS; ++i)
        {
            state->bandFreqs[i] = freqs[i];
            state->gainsDB[i] = gains[i];
            state->qValues[i] = qs[i];
        }
    }

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

void play_dsp_process(play_dsp_state* state, float* interleavedFrames, uint32_t frameCount, uint32_t channels)
{
    for (uint32_t i = 0; i < frameCount; ++i)
    {
        float mono = 0.0f;

        for (uint32_t ch = 0; ch < channels; ++ch)
        {
            float in = interleavedFrames[i * channels + ch];

            if (ch < MAX_CHANNELS)
            {
                for (int band = 0; band < EQ_BANDS; ++band)
                {
                    double out = state->b0[band] * in + state->b1[band] * state->x1[band][ch] + state->b2[band] * state->x2[band][ch]
                               - state->a1[band] * state->y1[band][ch] - state->a2[band] * state->y2[band][ch];

                    state->x2[band][ch] = state->x1[band][ch];
                    state->x1[band][ch] = in;
                    state->y2[band][ch] = state->y1[band][ch];
                    state->y1[band][ch] = out;
                    in = (float)out;
                }
            }

            interleavedFrames[i * channels + ch] = in;
            mono += in;
        }

        mono /= (float)channels;
        state->samples[state->writeIndex++] = mono;

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
        outBins[i] = state->bins[i];
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
