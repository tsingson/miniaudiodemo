#include "play_dsp_common.h"
#include "play_eq_linear.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail = 0;

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

static void test_map_gain_identity(void)
{
    CHECK_NEAR(play_eq_linear_map_gain_db(3.0), 3.0, 1e-9, "linear_map_plus3_identity");
    CHECK_NEAR(play_eq_linear_map_gain_db(-3.0), -3.0, 1e-9, "linear_map_minus3_identity");
}

static void test_config_sanitize(void)
{
    play_dsp_state s;
    int en = 0;
    int taps = 0;

    memset(&s, 0, sizeof(s));
    s.sampleRate = 48000;
    play_eq_linear_init(&s);

    play_eq_linear_set_config(&s, 0, 1);
    play_eq_linear_get_config(&s, &en, &taps, NULL, NULL);
    CHECK_TRUE(en == 0, "linear_config_enabled_off", "");
    CHECK_TRUE(taps == 17, "linear_config_taps_sanitize_17", "");

    play_eq_linear_set_config(&s, 1, 20);
    play_eq_linear_get_config(&s, &en, &taps, NULL, NULL);
    CHECK_TRUE(en == 1, "linear_config_enabled_on", "");
    CHECK_TRUE(taps == 25, "linear_config_taps_sanitize_25", "");

    play_eq_linear_set_config(&s, 1, 99);
    play_eq_linear_get_config(&s, &en, &taps, NULL, NULL);
    CHECK_TRUE(taps == 33, "linear_config_taps_sanitize_33", "");
}

static void test_process_passthrough_when_disabled(void)
{
    play_dsp_state s;
    float x = 0.1234f;
    float y;

    memset(&s, 0, sizeof(s));
    s.sampleRate = 48000;
    play_eq_linear_init(&s);
    s.linearFirEnabled = 0u;

    y = play_eq_linear_process_sample(&s, 0u, x);
    CHECK_NEAR(y, x, 1e-9, "linear_process_passthrough_disabled");
}

static void test_rebuild_enable_disable_with_dsp(void)
{
    play_dsp_state s;
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];

    memset(gains, 0, sizeof(gains));
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        qs[i] = 0.9f;
    }

    if (play_dsp_init(&s, 48000, 1) != 0)
    {
        printf("FAIL linear_rebuild_dsp_init\\n");
        g_fail++;
        return;
    }

    gains[0] = 3.0f;
    play_dsp_set_linear_fir_config(&s, 1, 25);
    play_dsp_set_eq_params(&s, gains, EQ_BANDS, qs, EQ_BANDS);
    CHECK_TRUE(s.linearFirEnabled != 0u, "linear_rebuild_enabled_with_linear_gain", "");

    play_dsp_set_linear_fir_config(&s, 0, 25);
    CHECK_TRUE(s.linearFirEnabled == 0u, "linear_rebuild_disabled_by_user", "");
}

int main(void)
{
    test_map_gain_identity();
    test_config_sanitize();
    test_process_passthrough_when_disabled();
    test_rebuild_enable_disable_with_dsp();

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
