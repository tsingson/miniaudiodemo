#include "play_eq_dynamic.h"

#include "play_dsp_common.h"

#include <stddef.h>

#define DYNAMIC_EQ_DEFAULT_ATTACK 0.12f
#define DYNAMIC_EQ_DEFAULT_RELEASE 0.02f
#define DYNAMIC_EQ_DEFAULT_THRESHOLD 0.18f
#define DYNAMIC_EQ_DEFAULT_MAX_REDUCTION_DB 12.0f
#define DYNAMIC_EQ_DEFAULT_STRENGTH_DB 18.0f

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

float play_eq_dynamic_update_env(float env, float absSample, float attack, float release)
{
    float coeff = (absSample > env) ? attack : release;
    return env + coeff * (absSample - env);
}

float play_eq_dynamic_compute_reduction(float env,
                                        float threshold,
                                        float strengthDb,
                                        float maxReductionDb,
                                        int hasPositiveGain)
{
    float reductionDb = 0.0f;

    if (!hasPositiveGain || env <= threshold)
    {
        return 0.0f;
    }

    reductionDb = (env - threshold) * strengthDb;
    if (reductionDb > maxReductionDb)
    {
        reductionDb = maxReductionDb;
    }

    return reductionDb;
}

void play_eq_dynamic_init_default_params(play_eq_dynamic_params* params)
{
    if (params == NULL)
    {
        return;
    }

    params->attack = DYNAMIC_EQ_DEFAULT_ATTACK;
    params->release = DYNAMIC_EQ_DEFAULT_RELEASE;
    params->threshold = DYNAMIC_EQ_DEFAULT_THRESHOLD;
    params->maxReductionDB = DYNAMIC_EQ_DEFAULT_MAX_REDUCTION_DB;
    params->strengthDB = DYNAMIC_EQ_DEFAULT_STRENGTH_DB;
}

void play_eq_dynamic_set_params(play_eq_dynamic_params* params,
                                float attack,
                                float release,
                                float threshold,
                                float maxReductionDB,
                                float strengthDB)
{
    if (params == NULL)
    {
        return;
    }

    params->attack = clampf(attack, DYNAMIC_EQ_MIN_ATTACK, DYNAMIC_EQ_MAX_ATTACK);
    params->release = clampf(release, DYNAMIC_EQ_MIN_RELEASE, DYNAMIC_EQ_MAX_RELEASE);
    params->threshold = clampf(threshold, DYNAMIC_EQ_MIN_THRESHOLD, DYNAMIC_EQ_MAX_THRESHOLD);
    params->maxReductionDB = clampf(maxReductionDB, DYNAMIC_EQ_MIN_MAX_REDUCTION_DB, DYNAMIC_EQ_MAX_MAX_REDUCTION_DB);
    params->strengthDB = clampf(strengthDB, DYNAMIC_EQ_MIN_STRENGTH_DB, DYNAMIC_EQ_MAX_STRENGTH_DB);
}

void play_eq_dynamic_get_params(const play_eq_dynamic_params* params,
                                float* outAttack,
                                float* outRelease,
                                float* outThreshold,
                                float* outMaxReductionDB,
                                float* outStrengthDB)
{
    if (params == NULL)
    {
        return;
    }

    if (outAttack != NULL)
    {
        *outAttack = params->attack;
    }
    if (outRelease != NULL)
    {
        *outRelease = params->release;
    }
    if (outThreshold != NULL)
    {
        *outThreshold = params->threshold;
    }
    if (outMaxReductionDB != NULL)
    {
        *outMaxReductionDB = params->maxReductionDB;
    }
    if (outStrengthDB != NULL)
    {
        *outStrengthDB = params->strengthDB;
    }
}

void play_eq_dynamic_init_params_state(play_dsp_state* state)
{
    play_eq_dynamic_params params;

    if (state == NULL)
    {
        return;
    }

    play_eq_dynamic_init_default_params(&params);
    state->dynamicAttack = params.attack;
    state->dynamicRelease = params.release;
    state->dynamicThreshold = params.threshold;
    state->dynamicMaxReductionDB = params.maxReductionDB;
    state->dynamicStrengthDB = params.strengthDB;
}

void play_eq_dynamic_set_params_state(play_dsp_state* state,
                                      float attack,
                                      float release,
                                      float threshold,
                                      float maxReductionDB,
                                      float strengthDB)
{
    play_eq_dynamic_params params;

    if (state == NULL)
    {
        return;
    }

    params.attack = state->dynamicAttack;
    params.release = state->dynamicRelease;
    params.threshold = state->dynamicThreshold;
    params.maxReductionDB = state->dynamicMaxReductionDB;
    params.strengthDB = state->dynamicStrengthDB;

    play_eq_dynamic_set_params(&params, attack, release, threshold, maxReductionDB, strengthDB);

    state->dynamicAttack = params.attack;
    state->dynamicRelease = params.release;
    state->dynamicThreshold = params.threshold;
    state->dynamicMaxReductionDB = params.maxReductionDB;
    state->dynamicStrengthDB = params.strengthDB;
}

void play_eq_dynamic_get_params_state(const play_dsp_state* state,
                                      float* outAttack,
                                      float* outRelease,
                                      float* outThreshold,
                                      float* outMaxReductionDB,
                                      float* outStrengthDB)
{
    play_eq_dynamic_params params;

    if (state == NULL)
    {
        return;
    }

    params.attack = state->dynamicAttack;
    params.release = state->dynamicRelease;
    params.threshold = state->dynamicThreshold;
    params.maxReductionDB = state->dynamicMaxReductionDB;
    params.strengthDB = state->dynamicStrengthDB;

    play_eq_dynamic_get_params(&params, outAttack, outRelease, outThreshold, outMaxReductionDB, outStrengthDB);
}
