#ifndef PLAY_EQ_DYNAMIC_H
#define PLAY_EQ_DYNAMIC_H

typedef struct
{
    /* Envelope coefficients and threshold are normalized to sample magnitude [0, 1]. */
    float attack;
    float release;
    float threshold;
    /* Reduction/strength are in dB units. */
    float maxReductionDB;
    float strengthDB;
} play_eq_dynamic_params;

typedef struct play_dsp_state play_dsp_state;

float play_eq_dynamic_update_env(float env, float absSample, float attack, float release);
/* Returns positive attenuation value in dB; caller applies it as a negative gain. */
float play_eq_dynamic_compute_reduction(float env,
                                        float threshold,
                                        float strengthDb,
                                        float maxReductionDb,
                                        int hasPositiveGain);

void play_eq_dynamic_init_default_params(play_eq_dynamic_params* params);
void play_eq_dynamic_set_params(play_eq_dynamic_params* params,
                                float attack,
                                float release,
                                float threshold,
                                float maxReductionDB,
                                float strengthDB);
void play_eq_dynamic_get_params(const play_eq_dynamic_params* params,
                                float* outAttack,
                                float* outRelease,
                                float* outThreshold,
                                float* outMaxReductionDB,
                                float* outStrengthDB);

void play_eq_dynamic_init_params_state(play_dsp_state* state);
void play_eq_dynamic_set_params_state(play_dsp_state* state,
                                      float attack,
                                      float release,
                                      float threshold,
                                      float maxReductionDB,
                                      float strengthDB);
void play_eq_dynamic_get_params_state(const play_dsp_state* state,
                                      float* outAttack,
                                      float* outRelease,
                                      float* outThreshold,
                                      float* outMaxReductionDB,
                                      float* outStrengthDB);

#endif
