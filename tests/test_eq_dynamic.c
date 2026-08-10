#include "play_eq_dynamic.h"

#include <math.h>
#include <stdio.h>

static int g_fail = 0;

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
    play_eq_dynamic_params params;
    float attack = 0.0f;
    float release = 0.0f;
    float threshold = 0.0f;
    float maxReductionDb = 0.0f;
    float strengthDb = 0.0f;

    play_eq_dynamic_init_default_params(&params);
    play_eq_dynamic_get_params(&params, &attack, &release, &threshold, &maxReductionDb, &strengthDb);
    CHECK_NEAR(attack, 0.12f, 1e-6f, "params_default_attack");
    CHECK_NEAR(release, 0.02f, 1e-6f, "params_default_release");
    CHECK_NEAR(threshold, 0.18f, 1e-6f, "params_default_threshold");

    play_eq_dynamic_set_params(&params, 9.0f, 0.0001f, 2.0f, 99.0f, -3.0f);
    play_eq_dynamic_get_params(&params, &attack, &release, &threshold, &maxReductionDb, &strengthDb);
    CHECK_NEAR(attack, 0.50f, 1e-6f, "params_clamp_attack_max");
    CHECK_NEAR(release, 0.005f, 1e-6f, "params_clamp_release_min");
    CHECK_NEAR(threshold, 1.0f, 1e-6f, "params_clamp_threshold_max");
    CHECK_NEAR(maxReductionDb, 36.0f, 1e-6f, "params_clamp_max_reduction_max");
    CHECK_NEAR(strengthDb, 1.0f, 1e-6f, "params_clamp_strength_min");

    float envRise = play_eq_dynamic_update_env(0.10f, 0.50f, 0.20f, 0.05f);
    float envFall = play_eq_dynamic_update_env(0.10f, 0.00f, 0.20f, 0.05f);

    CHECK_NEAR(envRise, 0.18f, 1e-6f, "update_env_rise_uses_attack");
    CHECK_NEAR(envFall, 0.095f, 1e-6f, "update_env_fall_uses_release");

    CHECK_NEAR(play_eq_dynamic_compute_reduction(0.40f, 0.20f, 18.0f, 12.0f, 0),
               0.0f,
               1e-6f,
               "reduction_requires_positive_gain");

    CHECK_NEAR(play_eq_dynamic_compute_reduction(0.10f, 0.20f, 18.0f, 12.0f, 1),
               0.0f,
               1e-6f,
               "reduction_below_threshold_zero");

    CHECK_NEAR(play_eq_dynamic_compute_reduction(0.30f, 0.20f, 18.0f, 12.0f, 1),
               1.8f,
               1e-6f,
               "reduction_linear_region");

    CHECK_NEAR(play_eq_dynamic_compute_reduction(1.00f, 0.20f, 50.0f, 12.0f, 1),
               12.0f,
               1e-6f,
               "reduction_clamped_by_max");

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
