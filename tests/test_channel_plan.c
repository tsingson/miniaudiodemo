#include "play_channel_plan.h"

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

static void test_prepare_stereo_to_5_1(void)
{
    play_channel_plan plan;

    CHECK_TRUE(play_channel_plan_prepare_stereo_to_5_1(&plan) == 0, "prepare_stereo_to_5_1", "");
    CHECK_TRUE(plan.inChannels == 2u, "plan_in_channels_2", "");
    CHECK_TRUE(plan.outChannels == 6u, "plan_out_channels_6", "");
    CHECK_NEAR(plan.matrix[0][0], 1.0f, 1e-6, "m_L_from_L");
    CHECK_NEAR(plan.matrix[1][1], 1.0f, 1e-6, "m_R_from_R");
    CHECK_NEAR(plan.matrix[2][0], 0.7071068f, 1e-6, "m_C_from_L");
    CHECK_NEAR(plan.matrix[2][1], 0.7071068f, 1e-6, "m_C_from_R");
    CHECK_NEAR(plan.matrix[3][0], 0.0f, 1e-6, "m_LFE_from_L");
    CHECK_NEAR(plan.matrix[3][1], 0.0f, 1e-6, "m_LFE_from_R");
    CHECK_NEAR(plan.matrix[4][0], 0.7071068f, 1e-6, "m_Ls_from_L");
    CHECK_NEAR(plan.matrix[5][1], 0.7071068f, 1e-6, "m_Rs_from_R");
}

static void test_apply_planar_stereo_to_5_1(void)
{
    enum
    {
        FRAMES = 4
    };
    play_channel_plan plan;
    float inL[FRAMES] = {1.0f, 0.5f, -0.5f, -1.0f};
    float inR[FRAMES] = {0.0f, 0.25f, -0.25f, 0.0f};
    float outL[FRAMES] = {0};
    float outR[FRAMES] = {0};
    float outC[FRAMES] = {0};
    float outLfe[FRAMES] = {0};
    float outLs[FRAMES] = {0};
    float outRs[FRAMES] = {0};
    float* inPlanar[2] = {inL, inR};
    float* outPlanar[6] = {outL, outR, outC, outLfe, outLs, outRs};

    CHECK_TRUE(play_channel_plan_prepare_stereo_to_5_1(&plan) == 0, "prepare_before_apply", "");
    CHECK_TRUE(play_channel_plan_apply_planar(&plan, inPlanar, outPlanar, FRAMES) == 0, "apply_planar", "");

    for (int i = 0; i < FRAMES; ++i)
    {
        char name[64];
        snprintf(name, sizeof(name), "outL_%d", i);
        CHECK_NEAR(outL[i], inL[i], 1e-6, name);
        snprintf(name, sizeof(name), "outR_%d", i);
        CHECK_NEAR(outR[i], inR[i], 1e-6, name);
        snprintf(name, sizeof(name), "outC_%d", i);
        CHECK_NEAR(outC[i], 0.7071068f * (inL[i] + inR[i]), 1e-6, name);
        snprintf(name, sizeof(name), "outLfe_%d", i);
        CHECK_NEAR(outLfe[i], 0.0f, 1e-6, name);
        snprintf(name, sizeof(name), "outLs_%d", i);
        CHECK_NEAR(outLs[i], 0.7071068f * inL[i], 1e-6, name);
        snprintf(name, sizeof(name), "outRs_%d", i);
        CHECK_NEAR(outRs[i], 0.7071068f * inR[i], 1e-6, name);
    }
}

static void test_identity_plan(void)
{
    enum
    {
        FRAMES = 3
    };
    play_channel_plan plan;
    float in0[FRAMES] = {0.1f, 0.2f, 0.3f};
    float in1[FRAMES] = {0.4f, 0.5f, 0.6f};
    float out0[FRAMES] = {0};
    float out1[FRAMES] = {0};
    float* inPlanar[2] = {in0, in1};
    float* outPlanar[2] = {out0, out1};

    play_channel_plan_init_identity(&plan, 2u);
    CHECK_TRUE(play_channel_plan_apply_planar(&plan, inPlanar, outPlanar, FRAMES) == 0, "identity_apply", "");

    for (int i = 0; i < FRAMES; ++i)
    {
        char name[64];
        snprintf(name, sizeof(name), "identity_ch0_%d", i);
        CHECK_NEAR(out0[i], in0[i], 1e-6, name);
        snprintf(name, sizeof(name), "identity_ch1_%d", i);
        CHECK_NEAR(out1[i], in1[i], 1e-6, name);
    }
}

int main(void)
{
    test_prepare_stereo_to_5_1();
    test_apply_planar_stereo_to_5_1();
    test_identity_plan();

    if (g_fail == 0)
    {
        printf("summary: failed=0\\n");
        return 0;
    }

    printf("summary: failed=%d\\n", g_fail);
    return 1;
}
