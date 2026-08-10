#include "eq_low_seq.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SR 48000u
#define CH 1u
#define WARMUP 4096u
#define MEASURE 16384u

typedef struct
{
    double re;
    double im;
} cpx;

static int g_fail = 0;

static void check_true(int cond, const char* name, const char* detail)
{
    if (!cond)
    {
        printf("FAIL %s %s\n", name, detail);
        g_fail += 1;
    }
    else
    {
        printf("PASS %s\n", name);
    }
}

static double tone_gain_db(eq_low_seq_ctx* ctx, float freqHz)
{
    cpx inAcc = {0.0, 0.0};
    cpx outAcc = {0.0, 0.0};
    double ph = 0.0;
    double step = 2.0 * M_PI * (double)freqHz / (double)SR;
    float amp = 0.1f;

    memset(ctx->x1, 0, sizeof(ctx->x1));
    memset(ctx->x2, 0, sizeof(ctx->x2));
    memset(ctx->y1, 0, sizeof(ctx->y1));
    memset(ctx->y2, 0, sizeof(ctx->y2));

    for (uint32_t i = 0; i < WARMUP; ++i)
    {
        float x = amp * (float)sin(ph);
        (void)eq_low_seq_process_sample(ctx, 0u, x);
        ph += step;
        if (ph > 2.0 * M_PI)
        {
            ph -= 2.0 * M_PI;
        }
    }

    for (uint32_t i = 0; i < MEASURE; ++i)
    {
        float x = amp * (float)sin(ph);
        float y = eq_low_seq_process_sample(ctx, 0u, x);
        double a = step * (double)i;
        inAcc.re += (double)x * cos(a);
        inAcc.im -= (double)x * sin(a);
        outAcc.re += (double)y * cos(a);
        outAcc.im -= (double)y * sin(a);

        ph += step;
        if (ph > 2.0 * M_PI)
        {
            ph -= 2.0 * M_PI;
        }
    }

    {
        double inMag = sqrt(inAcc.re * inAcc.re + inAcc.im * inAcc.im) + 1e-12;
        double outMag = sqrt(outAcc.re * outAcc.re + outAcc.im * outAcc.im) + 1e-12;
        return 20.0 * log10(outMag / inMag);
    }
}

static void test_effective_gain_under_150(void)
{
    eq_low_seq_ctx ctx;
    const float freqs[] = {35.0f, 60.0f, 110.0f};

    eq_low_seq_init(&ctx, SR, CH);
    eq_low_seq_set_mode(&ctx, EQ_LOW_SEQ_MODE_GAIN_FOCUSED);

    for (int i = 0; i < 3; ++i)
    {
        eq_low_seq_set_band(&ctx, i, freqs[i], 9.0f, 4.0f, 1);
    }

    for (int i = 0; i < 3; ++i)
    {
        double g = tone_gain_db(&ctx, freqs[i]);
        char detail[128];
        snprintf(detail, sizeof(detail), "freq=%.1f gain=%.3fdB", freqs[i], g);
        check_true(g > 6.5, "low_seq_effective_gain_under150", detail);
    }
}

static void test_narrowband_behavior(void)
{
    eq_low_seq_ctx ctx;
    double gCenter;
    double gNear;
    double gFar;

    eq_low_seq_init(&ctx, SR, CH);
    eq_low_seq_set_mode(&ctx, EQ_LOW_SEQ_MODE_GAIN_FOCUSED);
    eq_low_seq_set_band(&ctx, 0, 110.0f, 12.0f, 5.0f, 1);

    gCenter = tone_gain_db(&ctx, 110.0f);
    gNear = tone_gain_db(&ctx, 150.0f);
    gFar = tone_gain_db(&ctx, 300.0f);

    {
        char detail[160];
        snprintf(detail, sizeof(detail), "center=%.2f near=%.2f far=%.2f", gCenter, gNear, gFar);
        check_true((gCenter - gNear) > 3.0 && (gCenter - gFar) > 6.0, "low_seq_narrowband_shape", detail);
    }
}

static void test_latency_and_phase_mode(void)
{
    eq_low_seq_ctx ctx;
    int dSamples = -1;
    float dMs = -1.0f;

    eq_low_seq_init(&ctx, SR, CH);
    eq_low_seq_set_band(&ctx, 0, 120.0f, 12.0f, 10.0f, 1);

    eq_low_seq_get_latency(&ctx, &dSamples, &dMs);
    check_true(dSamples == 0 && fabsf(dMs) < 1e-6f, "low_seq_zero_algo_delay", "");

    eq_low_seq_set_mode(&ctx, EQ_LOW_SEQ_MODE_PHASE_FOCUSED);
    {
        float qCap = eq_low_seq_q_cap_for_freq(EQ_LOW_SEQ_MODE_PHASE_FOCUSED, 120.0f);
        char detail[128];
        snprintf(detail, sizeof(detail), "q=%.3f cap=%.3f", ctx.bands[0].q, qCap);
        check_true(ctx.bands[0].q <= qCap + 1e-5f, "low_seq_phase_mode_q_limit", detail);
    }
}

int main(void)
{
    test_effective_gain_under_150();
    test_narrowband_behavior();
    test_latency_and_phase_mode();

    printf("summary: failed=%d\n", g_fail);
    return (g_fail == 0) ? 0 : 1;
}
