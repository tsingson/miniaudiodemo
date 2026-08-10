#include "miniaudio/miniaudio.h"
#include "play_eq_linear.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SR 48000u
#define TEST_FREQ_COUNT 5
#define TEST_SECONDS 4u
#define TEST_FRAMES ((size_t)TEST_SR * TEST_SECONDS)
#define TEST_SKIP 4096u
#define TEST_RING_FRAMES 8192u
#define TEST_RING_STEP_AT 1024u
#define WAV_BLOCK_FRAMES 1024u

typedef struct
{
    double re;
    double im;
} cpx;

typedef struct
{
    uint32_t rng;
    double b0;
    double b1;
    double b2;
    double b3;
    double b4;
    double b5;
    double b6;
} pink_noise_state;

static const double g_testFreqs[TEST_FREQ_COUNT] = {35.0, 60.0, 110.0, 220.0, 300.0};
static const double g_testBoosts[] = {3.0, 6.0, 12.0};

static int g_failCount = 0;

static void check_true(int cond, const char* name, const char* detail)
{
    if (!cond)
    {
        printf("FAIL %s %s\n", name, detail);
        g_failCount += 1;
    }
    else
    {
        printf("PASS %s\n", name);
    }
}

static double phase_wrap(double x)
{
    while (x > M_PI)
    {
        x -= 2.0 * M_PI;
    }
    while (x < -M_PI)
    {
        x += 2.0 * M_PI;
    }
    return x;
}

static void pink_noise_init(pink_noise_state* st, uint32_t seed)
{
    memset(st, 0, sizeof(*st));
    st->rng = seed;
}

static double white_rand(pink_noise_state* st)
{
    st->rng = st->rng * 1664525u + 1013904223u;
    return ((double)((int32_t)st->rng) / 2147483648.0);
}

static double pink_rand(pink_noise_state* st)
{
    double white = white_rand(st);
    st->b0 = 0.99886 * st->b0 + white * 0.0555179;
    st->b1 = 0.99332 * st->b1 + white * 0.0750759;
    st->b2 = 0.96900 * st->b2 + white * 0.1538520;
    st->b3 = 0.86650 * st->b3 + white * 0.3104856;
    st->b4 = 0.55000 * st->b4 + white * 0.5329522;
    st->b5 = -0.7616 * st->b5 - white * 0.0168980;

    {
        double out = st->b0 + st->b1 + st->b2 + st->b3 + st->b4 + st->b5 + st->b6 + white * 0.5362;
        st->b6 = white * 0.115926;
        return out * 0.12;
    }
}

static void generate_pink_multitone(float* out, size_t frames, uint32_t sampleRate)
{
    pink_noise_state noise;
    double phase[TEST_FREQ_COUNT] = {0.0};
    double step[TEST_FREQ_COUNT];

    pink_noise_init(&noise, 0x13579BDFu);

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        step[i] = 2.0 * M_PI * g_testFreqs[i] / (double)sampleRate;
    }

    for (size_t n = 0; n < frames; ++n)
    {
        double tones = 0.0;
        for (int i = 0; i < TEST_FREQ_COUNT; ++i)
        {
            tones += 0.10 * sin(phase[i]);
            phase[i] += step[i];
            if (phase[i] > 2.0 * M_PI)
            {
                phase[i] -= 2.0 * M_PI;
            }
        }
        out[n] = (float)(tones + pink_rand(&noise));
    }
}

static void generate_multitone_only(float* out, size_t frames, uint32_t sampleRate)
{
    double phase[TEST_FREQ_COUNT] = {0.0};
    double step[TEST_FREQ_COUNT];

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        step[i] = 2.0 * M_PI * g_testFreqs[i] / (double)sampleRate;
    }

    for (size_t n = 0; n < frames; ++n)
    {
        double tones = 0.0;
        for (int i = 0; i < TEST_FREQ_COUNT; ++i)
        {
            tones += 0.12 * sin(phase[i]);
            phase[i] += step[i];
            if (phase[i] > 2.0 * M_PI)
            {
                phase[i] -= 2.0 * M_PI;
            }
        }
        out[n] = (float)tones;
    }
}

static cpx project_tone(const float* signal, size_t start, size_t frames, double freq, double sampleRate)
{
    cpx acc = {0.0, 0.0};
    double w = 2.0 * M_PI * freq / sampleRate;

    for (size_t i = start; i < frames; ++i)
    {
        double a = w * (double)(i - start);
        double c = cos(a);
        double s = sin(a);
        acc.re += (double)signal[i] * c;
        acc.im -= (double)signal[i] * s;
    }

    return acc;
}

static cpx cpx_div(cpx a, cpx b)
{
    cpx out;
    double d = b.re * b.re + b.im * b.im + 1e-20;
    out.re = (a.re * b.re + a.im * b.im) / d;
    out.im = (a.im * b.re - a.re * b.im) / d;
    return out;
}

static double cpx_mag(cpx v)
{
    return sqrt(v.re * v.re + v.im * v.im);
}

static void process_planar_mono_with_linear_ctx(play_eq_linear_ctx* ctx, float* mono, size_t frames)
{
    for (size_t i = 0; i < frames; ++i)
    {
        mono[i] = play_eq_linear_ctx_process_sample(ctx, 0u, mono[i]);
    }
}

static void build_linear_ctx(play_eq_linear_ctx* ctx, uint32_t sampleRate, double boostDb)
{
    float freqs[TEST_FREQ_COUNT];
    float gains[TEST_FREQ_COUNT];
    float qs[TEST_FREQ_COUNT];
    uint8_t modes[TEST_FREQ_COUNT];

    play_eq_linear_ctx_init(ctx, sampleRate, 1u);
    play_eq_linear_ctx_set_config(ctx, 1, 33);

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        freqs[i] = (float)g_testFreqs[i];
        gains[i] = (float)boostDb;
        qs[i] = 4.0f;
        modes[i] = 1u;
    }

    play_eq_linear_ctx_rebuild(ctx, freqs, gains, qs, modes, TEST_FREQ_COUNT, 1u);
}

static void test_ringing_and_latency(double boostDb)
{
    play_eq_linear_ctx ctx;
    float in[TEST_RING_FRAMES];
    float out[TEST_RING_FRAMES];
    int delaySamples = 0;
    float delayMs = 0.0f;
    double preMax = 0.0;
    double overshoot = 0.0;
    double undershoot = 0.0;

    build_linear_ctx(&ctx, TEST_SR, boostDb);
    play_eq_linear_ctx_get_config(&ctx, NULL, NULL, &delaySamples, &delayMs);

    check_true(delayMs <= 2.0f, "linear_delay_under_2ms", "");

    memset(in, 0, sizeof(in));
    for (size_t i = TEST_RING_STEP_AT; i < TEST_RING_FRAMES; ++i)
    {
        in[i] = 1.0f;
    }
    memcpy(out, in, sizeof(in));
    process_planar_mono_with_linear_ctx(&ctx, out, TEST_RING_FRAMES);

    for (size_t i = 0; i + 2 < TEST_RING_STEP_AT + (size_t)delaySamples; ++i)
    {
        double a = fabs((double)out[i]);
        if (a > preMax)
        {
            preMax = a;
        }
    }

    {
        size_t start = TEST_RING_STEP_AT + (size_t)delaySamples;
        size_t end = start + 512u;
        if (end > TEST_RING_FRAMES)
        {
            end = TEST_RING_FRAMES;
        }

        for (size_t i = start; i < end; ++i)
        {
            double y = (double)out[i];
            double over = y - 1.0;
            double under = 1.0 - y;
            if (over > overshoot)
            {
                overshoot = over;
            }
            if (under > undershoot)
            {
                undershoot = under;
            }
        }
    }

    {
        char detail[128];
        snprintf(detail, sizeof(detail), "preMax=%.4f over=%.4f under=%.4f", preMax, overshoot, undershoot);
        check_true(preMax < 0.12 && overshoot < 0.15 && undershoot < 0.15, "linear_step_ringing_low", detail);
    }
}

static void test_synthetic_phase_gain(double boostDb)
{
    play_eq_linear_ctx ctx;
    float in[TEST_FRAMES];
    float out[TEST_FRAMES];
    int delaySamples = 0;
    float delayMs = 0.0f;

    generate_multitone_only(in, TEST_FRAMES, TEST_SR);
    memcpy(out, in, sizeof(in));

    build_linear_ctx(&ctx, TEST_SR, boostDb);
    play_eq_linear_ctx_get_config(&ctx, NULL, NULL, &delaySamples, &delayMs);
    process_planar_mono_with_linear_ctx(&ctx, out, TEST_FRAMES);

    printf("synthetic_case boost=%.1fdB delaySamples=%d delayMs=%.4f\n", boostDb, delaySamples, delayMs);

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        cpx x = project_tone(in, TEST_SKIP, TEST_FRAMES, g_testFreqs[i], (double)TEST_SR);
        cpx y = project_tone(out, TEST_SKIP, TEST_FRAMES, g_testFreqs[i], (double)TEST_SR);
        cpx h = cpx_div(y, x);
        double gainDb = 20.0 * log10(cpx_mag(h) + 1e-12);
        double phase = atan2(h.im, h.re);
        double expected = phase_wrap(-2.0 * M_PI * g_testFreqs[i] * (double)delaySamples / (double)TEST_SR);
        double residual = fabs(phase_wrap(phase - expected));

        {
            char name[128];
            char detail[160];

            snprintf(name, sizeof(name), "linear_phase_%ghz_%gdb", g_testFreqs[i], boostDb);
            snprintf(detail, sizeof(detail), "residual=%.4frad expected=%.4f phase=%.4f", residual, expected, phase);
            check_true(residual < 0.20, name, detail);
            printf("INFO linear_gain_%ghz_%gdb measured=%.3fdB target=%.3fdB\n", g_testFreqs[i], boostDb, gainDb,
                   boostDb);
        }
    }
}

static void test_pink_generator_stability(double boostDb)
{
    play_eq_linear_ctx ctx;
    float in[TEST_FRAMES];
    float out[TEST_FRAMES];
    double inPow = 0.0;
    double outPow = 0.0;

    generate_pink_multitone(in, TEST_FRAMES, TEST_SR);
    memcpy(out, in, sizeof(in));

    build_linear_ctx(&ctx, TEST_SR, boostDb);
    process_planar_mono_with_linear_ctx(&ctx, out, TEST_FRAMES);

    for (size_t i = 0; i < TEST_FRAMES; ++i)
    {
        if (!isfinite(out[i]))
        {
            check_true(0, "pink_stability_finite", "NaN/Inf found");
            return;
        }
        inPow += (double)in[i] * (double)in[i];
        outPow += (double)out[i] * (double)out[i];
    }

    {
        double ratio = sqrt((outPow + 1e-12) / (inPow + 1e-12));
        char detail[128];
        snprintf(detail, sizeof(detail), "rms_ratio=%.4f", ratio);
        check_true(ratio > 0.4 && ratio < 2.5, "pink_stability_rms_ratio", detail);
    }
}

static void test_wav_mono_planar(double boostDb)
{
    ma_decoder decoder;
    ma_decoder_config cfg;
    play_eq_linear_ctx ctx;
    float inBuf[WAV_BLOCK_FRAMES];
    float outBuf[WAV_BLOCK_FRAMES];
    cpx xAcc[TEST_FREQ_COUNT];
    cpx yAcc[TEST_FREQ_COUNT];
    int validCount = 0;
    ma_result r;
    size_t total = 0;
    int delaySamples = 0;
    float delayMs = 0.0f;

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        xAcc[i].re = 0.0;
        xAcc[i].im = 0.0;
        yAcc[i].re = 0.0;
        yAcc[i].im = 0.0;
    }

    cfg = ma_decoder_config_init(ma_format_f32, 1, TEST_SR);
    r = ma_decoder_init_file("1.wav", &cfg, &decoder);
    if (r != MA_SUCCESS)
    {
        check_true(0, "wav_decoder_open", "failed to open 1.wav");
        return;
    }

    build_linear_ctx(&ctx, TEST_SR, boostDb);
    play_eq_linear_ctx_get_config(&ctx, NULL, NULL, &delaySamples, &delayMs);

    for (;;)
    {
        ma_uint64 framesRead = 0;
        r = ma_decoder_read_pcm_frames(&decoder, inBuf, WAV_BLOCK_FRAMES, &framesRead);
        if (r != MA_SUCCESS || framesRead == 0)
        {
            break;
        }

        memcpy(outBuf, inBuf, (size_t)framesRead * sizeof(float));
        process_planar_mono_with_linear_ctx(&ctx, outBuf, (size_t)framesRead);

        for (size_t i = 0; i < (size_t)framesRead; ++i)
        {
            if (!isfinite(outBuf[i]))
            {
                check_true(0, "wav_output_finite", "NaN/Inf found");
                ma_decoder_uninit(&decoder);
                return;
            }

            if (total >= TEST_SKIP)
            {
                for (int k = 0; k < TEST_FREQ_COUNT; ++k)
                {
                    double w = 2.0 * M_PI * g_testFreqs[k] / (double)TEST_SR;
                    double a = w * (double)(total - TEST_SKIP);
                    double c = cos(a);
                    double s = sin(a);

                    xAcc[k].re += (double)inBuf[i] * c;
                    xAcc[k].im -= (double)inBuf[i] * s;
                    yAcc[k].re += (double)outBuf[i] * c;
                    yAcc[k].im -= (double)outBuf[i] * s;
                }
            }
            total += 1;
        }

        if (total >= (size_t)TEST_SR * 20u)
        {
            break;
        }
    }

    ma_decoder_uninit(&decoder);

    check_true(delayMs <= 2.0f, "wav_linear_delay_under_2ms", "");
    check_true(total > TEST_SKIP + 4096u, "wav_enough_samples", "");

    for (int i = 0; i < TEST_FREQ_COUNT; ++i)
    {
        cpx h;
        double inMag = cpx_mag(xAcc[i]);
        double gainDb;
        double phase;
        double expected;
        double residual;
        char name[128];
        char detail[200];

        if (inMag < 5.0)
        {
            printf("SKIP wav_phase_%ghz_%gdb input_energy_low\n", g_testFreqs[i], boostDb);
            continue;
        }

        h = cpx_div(yAcc[i], xAcc[i]);
        gainDb = 20.0 * log10(cpx_mag(h) + 1e-12);
        phase = atan2(h.im, h.re);
        expected = phase_wrap(-2.0 * M_PI * g_testFreqs[i] * (double)delaySamples / (double)TEST_SR);
        residual = fabs(phase_wrap(phase - expected));

        snprintf(name, sizeof(name), "wav_phase_%ghz_%gdb", g_testFreqs[i], boostDb);
        snprintf(detail, sizeof(detail), "residual=%.4f gain=%.3fdB", residual, gainDb);
        check_true(residual < 0.40, name, detail);
        validCount += 1;
    }

    check_true(validCount >= 2, "wav_valid_phase_bins", "at least 2 low/mid bins need enough energy");
}

int main(void)
{
    printf("verify_eq_linear_mono: pink+multitone + wav mono planar checks\n");

    for (size_t i = 0; i < sizeof(g_testBoosts) / sizeof(g_testBoosts[0]); ++i)
    {
        double boost = g_testBoosts[i];
        test_synthetic_phase_gain(boost);
        test_pink_generator_stability(boost);
        test_ringing_and_latency(boost);
        test_wav_mono_planar(boost);
    }

    printf("summary: failed=%d\n", g_failCount);
    return (g_failCount == 0) ? 0 : 1;
}
