#include "play_eq_normal.h"

#include <math.h>
#include <stdio.h>

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

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
