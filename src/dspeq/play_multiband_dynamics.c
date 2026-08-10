#include "play_multiband_dynamics.h"

#include "play_dsp_common.h"

#include <math.h>
#include <stddef.h>

static const float kMbDynBandLowHz[PLAY_MB_DYN_BANDS] = {
    20.0f, 80.0f, 150.0f, 300.0f, 700.0f, 1500.0f, 4000.0f, 10000.0f
};
static const float kMbDynBandHighHz[PLAY_MB_DYN_BANDS] = {
    80.0f, 150.0f, 300.0f, 700.0f, 1500.0f, 4000.0f, 10000.0f, 16000.0f
};

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

static float db_to_linear(float db)
{
    return powf(10.0f, db / 20.0f);
}

static void update_multiband_dynamics_coeff(play_dsp_state* state)
{
    if (state == NULL || state->sampleRate == 0u)
    {
        return;
    }

    for (int i = 0; i < PLAY_MB_DYN_BANDS - 1; ++i)
    {
        float w = 2.0f * (float)PLAY_DSP_PI * kMbDynBandHighHz[i] / (float)state->sampleRate;
        state->mbDynLpA[i] = expf(-w);
    }
}

static float multiband_lowpass_process(play_dsp_state* state, int split, uint32_t ch, float x)
{
    float y = (1.0f - state->mbDynLpA[split]) * x + state->mbDynLpA[split] * state->mbDynLpState[split][ch];
    state->mbDynLpState[split][ch] = y;
    return y;
}

void play_mb_dyn_init(play_dsp_state* state)
{
    if (state == NULL)
    {
        return;
    }

    state->mbDynEnabled = 0u;
    state->mbDynPosition = (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    state->mbDynThreshold = PLAY_MB_DYN_DEFAULT_THRESHOLD;
    state->mbDynAttack = PLAY_MB_DYN_DEFAULT_ATTACK;
    state->mbDynRelease = PLAY_MB_DYN_DEFAULT_RELEASE;
    state->mbDynStrength = PLAY_MB_DYN_DEFAULT_STRENGTH;

    for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
    {
        state->mbDynBandLowHz[i] = kMbDynBandLowHz[i];
        state->mbDynBandHighHz[i] = kMbDynBandHighHz[i];
        state->mbDynBandAmountDb[i] = 0.0f;
        state->mbDynBandAppliedDb[i] = 0.0f;
    }

    update_multiband_dynamics_coeff(state);
}

void play_mb_dyn_set_config(play_dsp_state* state,
                            int enabled,
                            uint8_t position,
                            float threshold,
                            float attack,
                            float release,
                            float strength)
{
    if (state == NULL)
    {
        return;
    }

    state->mbDynEnabled = enabled ? 1u : 0u;
    state->mbDynPosition = (position == (uint8_t)PLAY_CROSSFEED_POST_EQ)
                               ? (uint8_t)PLAY_CROSSFEED_POST_EQ
                               : (uint8_t)PLAY_CROSSFEED_PRE_EQ;
    state->mbDynThreshold = clampf(threshold, PLAY_MB_DYN_MIN_THRESHOLD, PLAY_MB_DYN_MAX_THRESHOLD);
    state->mbDynAttack = clampf(attack, PLAY_MB_DYN_MIN_ATTACK, PLAY_MB_DYN_MAX_ATTACK);
    state->mbDynRelease = clampf(release, PLAY_MB_DYN_MIN_RELEASE, PLAY_MB_DYN_MAX_RELEASE);
    state->mbDynStrength = clampf(strength, PLAY_MB_DYN_MIN_STRENGTH, PLAY_MB_DYN_MAX_STRENGTH);
}

void play_mb_dyn_get_config(const play_dsp_state* state,
                            int* outEnabled,
                            uint8_t* outPosition,
                            float* outThreshold,
                            float* outAttack,
                            float* outRelease,
                            float* outStrength)
{
    if (state == NULL)
    {
        return;
    }

    if (outEnabled != NULL)
    {
        *outEnabled = state->mbDynEnabled ? 1 : 0;
    }
    if (outPosition != NULL)
    {
        *outPosition = state->mbDynPosition;
    }
    if (outThreshold != NULL)
    {
        *outThreshold = state->mbDynThreshold;
    }
    if (outAttack != NULL)
    {
        *outAttack = state->mbDynAttack;
    }
    if (outRelease != NULL)
    {
        *outRelease = state->mbDynRelease;
    }
    if (outStrength != NULL)
    {
        *outStrength = state->mbDynStrength;
    }
}

void play_mb_dyn_set_amounts(play_dsp_state* state, const float* amountsDb, int count)
{
    if (state == NULL || amountsDb == NULL || count <= 0)
    {
        return;
    }

    int n = (count < PLAY_MB_DYN_BANDS) ? count : PLAY_MB_DYN_BANDS;
    for (int i = 0; i < n; ++i)
    {
        state->mbDynBandAmountDb[i] = clampf(amountsDb[i], PLAY_MB_DYN_MIN_DB, PLAY_MB_DYN_MAX_DB);
    }
}

void play_mb_dyn_copy_amounts(const play_dsp_state* state, float* outAmountsDb, int maxCount)
{
    if (state == NULL || outAmountsDb == NULL || maxCount <= 0)
    {
        return;
    }

    int n = (maxCount < PLAY_MB_DYN_BANDS) ? maxCount : PLAY_MB_DYN_BANDS;
    for (int i = 0; i < n; ++i)
    {
        outAmountsDb[i] = state->mbDynBandAmountDb[i];
    }
}

void play_mb_dyn_copy_applied_db(const play_dsp_state* state, float* outAppliedDb, int maxCount)
{
    if (state == NULL || outAppliedDb == NULL || maxCount <= 0)
    {
        return;
    }

    int n = (maxCount < PLAY_MB_DYN_BANDS) ? maxCount : PLAY_MB_DYN_BANDS;
    for (int i = 0; i < n; ++i)
    {
        outAppliedDb[i] = state->mbDynBandAppliedDb[i];
    }
}

void play_mb_dyn_copy_bands(const play_dsp_state* state, float* outBandLowHz, float* outBandHighHz, int maxCount)
{
    if (state == NULL || maxCount <= 0)
    {
        return;
    }

    int n = (maxCount < PLAY_MB_DYN_BANDS) ? maxCount : PLAY_MB_DYN_BANDS;
    for (int i = 0; i < n; ++i)
    {
        if (outBandLowHz != NULL)
        {
            outBandLowHz[i] = state->mbDynBandLowHz[i];
        }
        if (outBandHighHz != NULL)
        {
            outBandHighHz[i] = state->mbDynBandHighHz[i];
        }
    }
}

void play_mb_dyn_process_sample(play_dsp_state* state, uint32_t ch, float* inOut)
{
    if (state == NULL || inOut == NULL || ch >= MAX_CHANNELS)
    {
        return;
    }

    float x = *inOut;
    float low[PLAY_MB_DYN_BANDS - 1];
    float bands[PLAY_MB_DYN_BANDS];
    float sum = 0.0f;

    for (int i = 0; i < PLAY_MB_DYN_BANDS - 1; ++i)
    {
        low[i] = multiband_lowpass_process(state, i, ch, x);
    }

    bands[0] = low[0];
    for (int i = 1; i < PLAY_MB_DYN_BANDS - 1; ++i)
    {
        bands[i] = low[i] - low[i - 1];
    }
    bands[PLAY_MB_DYN_BANDS - 1] = x - low[PLAY_MB_DYN_BANDS - 2];

    for (int i = 0; i < PLAY_MB_DYN_BANDS; ++i)
    {
        float sample = bands[i];
        float env = state->mbDynEnv[i][ch];
        float amountDb = state->mbDynBandAmountDb[i];
        float absSample = fabsf(sample);
        float coeff = (absSample > env) ? state->mbDynAttack : state->mbDynRelease;
        float appliedDb = 0.0f;

        env += coeff * (absSample - env);
        state->mbDynEnv[i][ch] = env;

        if (fabsf(amountDb) > 1e-4f && env > state->mbDynThreshold)
        {
            float over = env - state->mbDynThreshold;
            float delta = over * 24.0f * state->mbDynStrength;
            float maxDelta = fabsf(amountDb);
            if (delta > maxDelta)
            {
                delta = maxDelta;
            }
            appliedDb = (amountDb >= 0.0f) ? -delta : delta;
            sample *= db_to_linear(appliedDb);
        }

        state->mbDynBandAppliedDb[i] = state->mbDynBandAppliedDb[i] * 0.86f + appliedDb * 0.14f;
        sum += sample;
    }

    *inOut = sum;
}
