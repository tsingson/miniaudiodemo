#include "play_eq_normal.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define EQ_BANDS 16
#define MODE_LINEAR 0u
#define MODE_DYNAMIC 1u
#define MODE_NORMAL 2u

static int g_fail = 0;

static double biquad_mag(double b0, double b1, double b2, double a1, double a2, double w)
{
    double c1 = cos(w);
    double s1 = sin(w);
    double c2 = cos(2.0 * w);
    double s2 = sin(2.0 * w);
    double nr = b0 + b1 * c1 + b2 * c2;
    double ni = -(b1 * s1 + b2 * s2);
    double dr = 1.0 + a1 * c1 + a2 * c2;
    double di = -(a1 * s1 + a2 * s2);
    return sqrt((nr * nr + ni * ni) / (dr * dr + di * di + 1e-18));
}

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

int main(void)
{
    const double sr = 48000.0;
    double b0, b1, b2, a1, a2;
    float freqs[EQ_BANDS];
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    uint8_t modes[EQ_BANDS];
    uint8_t useSVF[EQ_BANDS];
    double rb0[EQ_BANDS];
    double rb1[EQ_BANDS];
    double rb2[EQ_BANDS];
    double ra1[EQ_BANDS];
    double ra2[EQ_BANDS];
    double svfG[EQ_BANDS];
    double svfK[EQ_BANDS];
    double svfA[EQ_BANDS];
    double svfH[EQ_BANDS];
    double ic1 = 0.0;
    double ic2 = 0.0;
    float svfOut;

    play_eq_normal_build_peaking(0.0, 0.9, 1000.0, sr, &b0, &b1, &b2, &a1, &a2);
    {
        double w = 2.0 * M_PI * 1000.0 / sr;
        double db = 20.0 * log10(biquad_mag(b0, b1, b2, a1, a2, w) + 1e-18);
        CHECK_NEAR(db, 0.0, 0.05, "peaking_0db_unity_at_center");
    }

    play_eq_normal_build_peaking(6.0, 0.9, 1000.0, sr, &b0, &b1, &b2, &a1, &a2);
    {
        double w = 2.0 * M_PI * 1000.0 / sr;
        double db = 20.0 * log10(biquad_mag(b0, b1, b2, a1, a2, w) + 1e-18);
        CHECK_NEAR(db, 6.0, 0.6, "peaking_plus6db_at_center");
    }

    play_eq_normal_build_lowpass_12db(5000.0, sr, 0.70710678118, &b0, &b1, &b2, &a1, &a2);
    {
        double wPass = 2.0 * M_PI * 1000.0 / sr;
        double wStop = 2.0 * M_PI * 20000.0 / sr;
        double passDb = 20.0 * log10(biquad_mag(b0, b1, b2, a1, a2, wPass) + 1e-18);
        double stopDb = 20.0 * log10(biquad_mag(b0, b1, b2, a1, a2, wStop) + 1e-18);
        CHECK_TRUE(passDb > -1.0 && passDb < 0.5, "lowpass_passband_reasonable", "(expect near 0dB)");
        CHECK_TRUE(stopDb < -8.0, "lowpass_stopband_attenuates", "(expect clearly below -8dB at 20k)");
    }

    memset(freqs, 0, sizeof(freqs));
    memset(gains, 0, sizeof(gains));
    memset(qs, 0, sizeof(qs));
    memset(modes, 0, sizeof(modes));
    memset(useSVF, 0, sizeof(useSVF));
    memset(rb0, 0, sizeof(rb0));
    memset(rb1, 0, sizeof(rb1));
    memset(rb2, 0, sizeof(rb2));
    memset(ra1, 0, sizeof(ra1));
    memset(ra2, 0, sizeof(ra2));
    memset(svfG, 0, sizeof(svfG));
    memset(svfK, 0, sizeof(svfK));
    memset(svfA, 0, sizeof(svfA));
    memset(svfH, 0, sizeof(svfH));

    play_eq_normal_init_default_profile_arrays(freqs, gains, qs, EQ_BANDS);
    CHECK_NEAR(freqs[0], 20.0f, 1e-6f, "default_profile_freq0");
    CHECK_NEAR(gains[15], -24.0f, 1e-6f, "default_profile_gain15");
    CHECK_NEAR(qs[7], 0.9f, 1e-6f, "default_profile_q7");

    play_eq_assign_band_modes_arrays(freqs,
                                     modes,
                                     EQ_BANDS,
                                     220.0f,
                                     16000.0f,
                                     MODE_LINEAR,
                                     MODE_DYNAMIC,
                                     MODE_NORMAL);
    CHECK_TRUE(modes[0] == MODE_LINEAR, "mode_assign_low_linear", "");
    CHECK_TRUE(modes[6] == MODE_DYNAMIC, "mode_assign_mid_dynamic", "");
    CHECK_TRUE(modes[15] == MODE_NORMAL, "mode_assign_high_normal", "");

    play_eq_normal_rebuild_arrays(48000u,
                                  freqs,
                                  gains,
                                  qs,
                                  modes,
                                  EQ_BANDS,
                                  MODE_LINEAR,
                                  1000.0f,
                                  useSVF,
                                  rb0,
                                  rb1,
                                  rb2,
                                  ra1,
                                  ra2,
                                  svfG,
                                  svfK,
                                  svfA,
                                  svfH);
    CHECK_TRUE(useSVF[5] == 1u, "rebuild_mid_uses_svf", "");
    CHECK_TRUE(useSVF[15] == 0u, "rebuild_high_uses_biquad", "");

    svfOut = play_eq_normal_process_svf_sample(svfG[5], svfK[5], svfA[5], svfH[5], &ic1, &ic2, 0.25f);
    CHECK_TRUE(isfinite(svfOut), "svf_sample_finite", "");

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
