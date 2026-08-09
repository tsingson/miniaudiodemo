#include "play_eq_linear.h"

#include "play_dsp_common.h"

#include <math.h>
#include <stddef.h>

#define PLAY_LINEAR_FIR_DESIGN_BINS 96

static int linear_taps_sanitize(int taps)
{
    if (taps <= 17)
    {
        return 17;
    }
    if (taps <= 25)
    {
        return 25;
    }
    return 33;
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

static double peaking_mag_at(double sampleRate, double f, double f0, double gainDb, double q)
{
    double A = pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * f0 / sampleRate;
    double alpha = sin(w0) / (2.0 * q);
    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cos(w0);
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cos(w0);
    double a2 = 1.0 - alpha / A;
    double w = 2.0 * M_PI * f / sampleRate;
    double c1 = cos(w);
    double s1 = sin(w);
    double c2 = cos(2.0 * w);
    double s2 = sin(2.0 * w);
    double nr = b0 + b1 * c1 + b2 * c2;
    double ni = -(b1 * s1 + b2 * s2);
    double dr = a0 + a1 * c1 + a2 * c2;
    double di = -(a1 * s1 + a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di + 1e-12));
}

double play_eq_linear_map_gain_db(double sliderGainDb)
{
    return sliderGainDb;
}

void play_eq_linear_init(play_dsp_state* state)
{
    state->linearFirEnabled = 0u;
    state->linearFirUserEnabled = 1u;
    state->linearFirTaps = (uint8_t)PLAY_LINEAR_FIR_DEFAULT_TAPS;
    for (int i = 0; i < PLAY_LINEAR_FIR_MAX_TAPS; ++i)
    {
        state->linearFirCoeff[i] = 0.0f;
        for (int ch = 0; ch < MAX_CHANNELS; ++ch)
        {
            state->linearFirHist[i][ch] = 0.0f;
        }
    }
    state->linearFirCoeff[(PLAY_LINEAR_FIR_DEFAULT_TAPS - 1) / 2] = 1.0f;
    for (int ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        state->linearFirWriteIndex[ch] = 0;
        state->linearFirTransientEnv[ch] = 0.0f;
        state->linearFirLastDelayed[ch] = 0.0f;
    }
}

void play_eq_linear_set_config(play_dsp_state* state, int enabled, int taps)
{
    state->linearFirUserEnabled = enabled ? 1u : 0u;
    state->linearFirTaps = (uint8_t)linear_taps_sanitize(taps);
}

void play_eq_linear_get_config(const play_dsp_state* state,
                               int* outEnabled,
                               int* outTaps,
                               int* outDelaySamples,
                               float* outDelayMs)
{
    int taps = linear_taps_sanitize((int)state->linearFirTaps);
    int delay = (taps - 1) / 2;

    if (outEnabled != NULL)
    {
        *outEnabled = state->linearFirUserEnabled ? 1 : 0;
    }
    if (outTaps != NULL)
    {
        *outTaps = taps;
    }
    if (outDelaySamples != NULL)
    {
        *outDelaySamples = delay;
    }
    if (outDelayMs != NULL)
    {
        *outDelayMs = (state->sampleRate > 0u) ? ((float)delay * 1000.0f / (float)state->sampleRate) : 0.0f;
    }
}

void play_eq_linear_rebuild(play_dsp_state* state)
{
    double mags[PLAY_LINEAR_FIR_DESIGN_BINS];
    double sumAbsLinear = 0.0;
    float a0Target = 1.0f;
    int taps = linear_taps_sanitize((int)state->linearFirTaps);
    int delay = (taps - 1) / 2;

    for (int i = 0; i < EQ_BANDS; ++i)
    {
        if (state->bandModes[i] == (uint8_t)PLAY_EQ_MODE_LINEAR)
        {
            sumAbsLinear += fabs((double)state->gainsDB[i]);
        }
    }

    if (!state->linearFirUserEnabled || sumAbsLinear < 1e-3)
    {
        state->linearFirEnabled = 0u;
        for (int i = 0; i < PLAY_LINEAR_FIR_MAX_TAPS; ++i)
        {
            state->linearFirCoeff[i] = 0.0f;
        }
        state->linearFirCoeff[delay] = 1.0f;
        return;
    }

    for (int k = 0; k < PLAY_LINEAR_FIR_DESIGN_BINS; ++k)
    {
        double w = M_PI * (double)k / (double)(PLAY_LINEAR_FIR_DESIGN_BINS - 1);
        double f = w * (double)state->sampleRate / (2.0 * M_PI);
        double mag = 1.0;

        for (int b = 0; b < EQ_BANDS; ++b)
        {
            if (state->bandModes[b] != (uint8_t)PLAY_EQ_MODE_LINEAR)
            {
                continue;
            }

            mag *= peaking_mag_at((double)state->sampleRate,
                                  f,
                                  (double)state->bandFreqs[b],
                                  play_eq_linear_map_gain_db((double)state->gainsDB[b]),
                                  (double)state->qValues[b]);
        }

        mags[k] = clampf((float)mag, 0.06f, 20.0f);
        if (k == 0)
        {
            a0Target = (float)mags[k];
        }
    }

    for (int n = 0; n < PLAY_LINEAR_FIR_MAX_TAPS; ++n)
    {
        state->linearFirCoeff[n] = 0.0f;
    }

    for (int n = 0; n < taps; ++n)
    {
        double acc = 0.0;
        double m = (double)n - (double)delay;
        for (int k = 0; k < PLAY_LINEAR_FIR_DESIGN_BINS; ++k)
        {
            double w = M_PI * (double)k / (double)(PLAY_LINEAR_FIR_DESIGN_BINS - 1);
            double wt = ((k == 0) || (k == PLAY_LINEAR_FIR_DESIGN_BINS - 1)) ? 0.5 : 1.0;
            acc += wt * mags[k] * cos(w * m);
        }

        acc /= (double)(PLAY_LINEAR_FIR_DESIGN_BINS - 1);
        acc *= (0.54 - 0.46 * cos((2.0 * M_PI * (double)n) / (double)(taps - 1)));
        state->linearFirCoeff[n] = (float)acc;
    }

    {
        double dc = 0.0;
        double scale;
        for (int i = 0; i < taps; ++i)
        {
            dc += state->linearFirCoeff[i];
        }
        scale = (fabs(dc) > 1e-9) ? ((double)a0Target / dc) : 1.0;
        for (int i = 0; i < taps; ++i)
        {
            state->linearFirCoeff[i] = (float)(state->linearFirCoeff[i] * scale);
        }
    }

    state->linearFirEnabled = 1u;
}

float play_eq_linear_process_sample(play_dsp_state* state, uint32_t ch, float in)
{
    float fir = 0.0f;
    int w;
    int taps;
    int delay;

    if (ch >= MAX_CHANNELS)
    {
        return in;
    }

    if (!state->linearFirEnabled)
    {
        return in;
    }

    taps = linear_taps_sanitize((int)state->linearFirTaps);
    delay = (taps - 1) / 2;

    w = state->linearFirWriteIndex[ch];
    state->linearFirHist[w][ch] = in;

    for (int t = 0; t < taps; ++t)
    {
        int idx = w - t;
        if (idx < 0)
        {
            idx += PLAY_LINEAR_FIR_MAX_TAPS;
        }
        fir += state->linearFirCoeff[t] * state->linearFirHist[idx][ch];
    }

    {
        int didx = w - delay;
        float delayed;
        float transient;
        float blend;

        if (didx < 0)
        {
            didx += PLAY_LINEAR_FIR_MAX_TAPS;
        }
        delayed = state->linearFirHist[didx][ch];

        transient = fabsf(delayed - state->linearFirLastDelayed[ch]);
        state->linearFirTransientEnv[ch] = state->linearFirTransientEnv[ch] * 0.86f + transient * 0.14f;
        state->linearFirLastDelayed[ch] = delayed;

        blend = clampf((state->linearFirTransientEnv[ch] - 0.0035f) * 36.0f, 0.0f, 0.30f);
        fir = fir + blend * (delayed - fir);
    }

    w += 1;
    if (w >= PLAY_LINEAR_FIR_MAX_TAPS)
    {
        w = 0;
    }
    state->linearFirWriteIndex[ch] = w;

    return fir;
}
