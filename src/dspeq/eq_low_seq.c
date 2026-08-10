#include "eq_low_seq.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EQ_LOW_SEQ_MIN_FREQ_HZ 20.0f
#define EQ_LOW_SEQ_MIN_Q 0.25f
#define EQ_LOW_SEQ_MAX_Q 12.0f
#define EQ_LOW_SEQ_MAX_GAIN_DB 18.0f

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

static uint32_t sanitize_channels(uint32_t channels)
{
    if (channels == 0u)
    {
        return 1u;
    }
    if (channels > EQ_LOW_SEQ_MAX_CHANNELS)
    {
        return EQ_LOW_SEQ_MAX_CHANNELS;
    }
    return channels;
}

float eq_low_seq_q_cap_for_freq(eq_low_seq_mode mode, float freqHz)
{
    float f = clampf(freqHz, EQ_LOW_SEQ_MIN_FREQ_HZ, EQ_LOW_SEQ_MAX_FREQ_HZ);

    if (mode == EQ_LOW_SEQ_MODE_PHASE_FOCUSED)
    {
        if (f < 80.0f)
        {
            return 2.4f;
        }
        if (f < 150.0f)
        {
            return 3.0f;
        }
        if (f < 300.0f)
        {
            return 3.8f;
        }
        return 4.5f;
    }

    if (mode == EQ_LOW_SEQ_MODE_BALANCED)
    {
        if (f < 80.0f)
        {
            return 3.6f;
        }
        if (f < 150.0f)
        {
            return 4.6f;
        }
        if (f < 300.0f)
        {
            return 5.6f;
        }
        return 6.5f;
    }

    if (f < 80.0f)
    {
        return 5.0f;
    }
    if (f < 150.0f)
    {
        return 6.5f;
    }
    if (f < 300.0f)
    {
        return 8.0f;
    }
    return 10.0f;
}

static float sanitize_q(eq_low_seq_mode mode, float freqHz, float q)
{
    float qCap = eq_low_seq_q_cap_for_freq(mode, freqHz);
    return clampf(q, EQ_LOW_SEQ_MIN_Q, clampf(qCap, EQ_LOW_SEQ_MIN_Q, EQ_LOW_SEQ_MAX_Q));
}

static void build_peaking(double sampleRate,
                          double f0,
                          double gainDb,
                          double q,
                          double* outB0,
                          double* outB1,
                          double* outB2,
                          double* outA1,
                          double* outA2)
{
    double A = pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * M_PI * f0 / sampleRate;
    double c = cos(w0);
    double s = sin(w0);
    double alpha = s / (2.0 * q);

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * c;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * c;
    double a2 = 1.0 - alpha / A;

    *outB0 = b0 / a0;
    *outB1 = b1 / a0;
    *outB2 = b2 / a0;
    *outA1 = a1 / a0;
    *outA2 = a2 / a0;
}

void eq_low_seq_init(eq_low_seq_ctx* ctx, uint32_t sampleRate, uint32_t channels)
{
    static const float defaultFreqs[EQ_LOW_SEQ_MAX_BANDS] = {35.0f, 60.0f, 90.0f, 120.0f, 150.0f, 220.0f, 300.0f,
                                                              420.0f};

    if (ctx == NULL)
    {
        return;
    }

    memset(ctx, 0, sizeof(*ctx));
    ctx->sampleRate = (sampleRate > 0u) ? sampleRate : 48000u;
    ctx->channels = sanitize_channels(channels);
    ctx->bandCount = EQ_LOW_SEQ_MAX_BANDS;
    ctx->bypass = 0u;
    ctx->mode = (uint8_t)EQ_LOW_SEQ_MODE_BALANCED;

    for (int i = 0; i < EQ_LOW_SEQ_MAX_BANDS; ++i)
    {
        ctx->bands[i].freqHz = defaultFreqs[i];
        ctx->bands[i].gainDb = 0.0f;
        ctx->bands[i].q = 2.0f;
        ctx->bands[i].enabled = 1u;
    }

    eq_low_seq_rebuild(ctx);
}

void eq_low_seq_set_bypass(eq_low_seq_ctx* ctx, int enabled)
{
    if (ctx == NULL)
    {
        return;
    }
    ctx->bypass = enabled ? 1u : 0u;
}

void eq_low_seq_set_mode(eq_low_seq_ctx* ctx, eq_low_seq_mode mode)
{
    if (ctx == NULL)
    {
        return;
    }

    if (mode < EQ_LOW_SEQ_MODE_GAIN_FOCUSED)
    {
        mode = EQ_LOW_SEQ_MODE_GAIN_FOCUSED;
    }
    if (mode > EQ_LOW_SEQ_MODE_PHASE_FOCUSED)
    {
        mode = EQ_LOW_SEQ_MODE_PHASE_FOCUSED;
    }

    ctx->mode = (uint8_t)mode;
    eq_low_seq_rebuild(ctx);
}

void eq_low_seq_set_band(eq_low_seq_ctx* ctx, int bandIndex, float freqHz, float gainDb, float q, int enabled)
{
    eq_low_seq_mode mode;

    if (ctx == NULL)
    {
        return;
    }
    if (bandIndex < 0 || bandIndex >= EQ_LOW_SEQ_MAX_BANDS)
    {
        return;
    }

    mode = (eq_low_seq_mode)ctx->mode;

    ctx->bands[bandIndex].freqHz = clampf(freqHz, EQ_LOW_SEQ_MIN_FREQ_HZ, EQ_LOW_SEQ_MAX_FREQ_HZ);
    ctx->bands[bandIndex].gainDb = clampf(gainDb, -EQ_LOW_SEQ_MAX_GAIN_DB, EQ_LOW_SEQ_MAX_GAIN_DB);
    ctx->bands[bandIndex].q = sanitize_q(mode, ctx->bands[bandIndex].freqHz, q);
    ctx->bands[bandIndex].enabled = enabled ? 1u : 0u;

    eq_low_seq_rebuild(ctx);
}

void eq_low_seq_set_profile(eq_low_seq_ctx* ctx,
                            const float* freqsHz,
                            const float* gainsDb,
                            const float* qs,
                            int count,
                            int enabled)
{
    int n;
    eq_low_seq_mode mode;

    if (ctx == NULL || count <= 0)
    {
        return;
    }

    n = (count > EQ_LOW_SEQ_MAX_BANDS) ? EQ_LOW_SEQ_MAX_BANDS : count;
    mode = (eq_low_seq_mode)ctx->mode;

    for (int i = 0; i < n; ++i)
    {
        if (freqsHz != NULL)
        {
            ctx->bands[i].freqHz = clampf(freqsHz[i], EQ_LOW_SEQ_MIN_FREQ_HZ, EQ_LOW_SEQ_MAX_FREQ_HZ);
        }
        if (gainsDb != NULL)
        {
            ctx->bands[i].gainDb = clampf(gainsDb[i], -EQ_LOW_SEQ_MAX_GAIN_DB, EQ_LOW_SEQ_MAX_GAIN_DB);
        }
        if (qs != NULL)
        {
            ctx->bands[i].q = sanitize_q(mode, ctx->bands[i].freqHz, qs[i]);
        }
        ctx->bands[i].enabled = enabled ? 1u : 0u;
    }

    eq_low_seq_rebuild(ctx);
}

void eq_low_seq_reset_state(eq_low_seq_ctx* ctx)
{
    if (ctx == NULL)
    {
        return;
    }

    for (int b = 0; b < EQ_LOW_SEQ_MAX_BANDS; ++b)
    {
        for (int ch = 0; ch < EQ_LOW_SEQ_MAX_CHANNELS; ++ch)
        {
            ctx->x1[b][ch] = 0.0;
            ctx->x2[b][ch] = 0.0;
            ctx->y1[b][ch] = 0.0;
            ctx->y2[b][ch] = 0.0;
        }
    }
}

void eq_low_seq_rebuild(eq_low_seq_ctx* ctx)
{
    eq_low_seq_mode mode;

    if (ctx == NULL || ctx->sampleRate == 0u)
    {
        return;
    }

    mode = (eq_low_seq_mode)ctx->mode;

    for (int i = 0; i < EQ_LOW_SEQ_MAX_BANDS; ++i)
    {
        float f;
        float g;
        float q;

        if (i >= (int)ctx->bandCount || !ctx->bands[i].enabled || fabsf(ctx->bands[i].gainDb) < 1e-4f)
        {
            ctx->b0[i] = 1.0;
            ctx->b1[i] = 0.0;
            ctx->b2[i] = 0.0;
            ctx->a1[i] = 0.0;
            ctx->a2[i] = 0.0;
            continue;
        }

        f = clampf(ctx->bands[i].freqHz, EQ_LOW_SEQ_MIN_FREQ_HZ, EQ_LOW_SEQ_MAX_FREQ_HZ);
        g = clampf(ctx->bands[i].gainDb, -EQ_LOW_SEQ_MAX_GAIN_DB, EQ_LOW_SEQ_MAX_GAIN_DB);
        q = sanitize_q(mode, f, ctx->bands[i].q);

        ctx->bands[i].freqHz = f;
        ctx->bands[i].gainDb = g;
        ctx->bands[i].q = q;

        build_peaking((double)ctx->sampleRate, (double)f, (double)g, (double)q, &ctx->b0[i], &ctx->b1[i],
                      &ctx->b2[i], &ctx->a1[i], &ctx->a2[i]);
    }
}

float eq_low_seq_process_sample(eq_low_seq_ctx* ctx, uint32_t ch, float in)
{
    double y = in;

    if (ctx == NULL || ctx->bypass)
    {
        return in;
    }

    if (ch >= sanitize_channels(ctx->channels))
    {
        return in;
    }

    for (int b = 0; b < (int)ctx->bandCount; ++b)
    {
        double x = y;

        y = ctx->b0[b] * x + ctx->b1[b] * ctx->x1[b][ch] + ctx->b2[b] * ctx->x2[b][ch] -
            ctx->a1[b] * ctx->y1[b][ch] - ctx->a2[b] * ctx->y2[b][ch];

        ctx->x2[b][ch] = ctx->x1[b][ch];
        ctx->x1[b][ch] = x;
        ctx->y2[b][ch] = ctx->y1[b][ch];
        ctx->y1[b][ch] = y;
    }

    return (float)y;
}

void eq_low_seq_process_planar(eq_low_seq_ctx* ctx, float* const* planar, uint32_t frameCount, uint32_t channels)
{
    uint32_t chn;

    if (ctx == NULL || planar == NULL)
    {
        return;
    }

    if (ctx->bypass)
    {
        return;
    }

    chn = sanitize_channels(channels);
    if (chn > sanitize_channels(ctx->channels))
    {
        chn = sanitize_channels(ctx->channels);
    }

    for (uint32_t ch = 0; ch < chn; ++ch)
    {
        if (planar[ch] == NULL)
        {
            continue;
        }

        for (uint32_t i = 0; i < frameCount; ++i)
        {
            planar[ch][i] = eq_low_seq_process_sample(ctx, ch, planar[ch][i]);
        }
    }
}

void eq_low_seq_get_latency(const eq_low_seq_ctx* ctx, int* outDelaySamples, float* outDelayMs)
{
    (void)ctx;

    if (outDelaySamples != NULL)
    {
        *outDelaySamples = 0;
    }
    if (outDelayMs != NULL)
    {
        *outDelayMs = 0.0f;
    }
}
