#ifndef PLAY_MULTIBAND_DYNAMICS_H
#define PLAY_MULTIBAND_DYNAMICS_H

#include <stdint.h>

typedef struct play_dsp_state play_dsp_state;

void play_mb_dyn_init(play_dsp_state* state);
void play_mb_dyn_set_config(play_dsp_state* state,
                            int enabled,
                            uint8_t position,
                            float threshold,
                            float attack,
                            float release,
                            float strength);
void play_mb_dyn_get_config(const play_dsp_state* state,
                            int* outEnabled,
                            uint8_t* outPosition,
                            float* outThreshold,
                            float* outAttack,
                            float* outRelease,
                            float* outStrength);
void play_mb_dyn_set_amounts(play_dsp_state* state, const float* amountsDb, int count);
void play_mb_dyn_copy_amounts(const play_dsp_state* state, float* outAmountsDb, int maxCount);
void play_mb_dyn_copy_applied_db(const play_dsp_state* state, float* outAppliedDb, int maxCount);
void play_mb_dyn_copy_bands(const play_dsp_state* state, float* outBandLowHz, float* outBandHighHz, int maxCount);
void play_mb_dyn_process_sample(play_dsp_state* state, uint32_t ch, float* inOut);

#endif
