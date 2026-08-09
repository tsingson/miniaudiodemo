#ifndef PLAY_EQ_LINEAR_H
#define PLAY_EQ_LINEAR_H

#include <stdint.h>

typedef struct play_dsp_state play_dsp_state;

double play_eq_linear_map_gain_db(double sliderGainDb);
void play_eq_linear_init(play_dsp_state* state);
void play_eq_linear_set_config(play_dsp_state* state, int enabled, int taps);
void play_eq_linear_get_config(const play_dsp_state* state,
							   int* outEnabled,
							   int* outTaps,
							   int* outDelaySamples,
							   float* outDelayMs);
void play_eq_linear_rebuild(play_dsp_state* state);
float play_eq_linear_process_sample(play_dsp_state* state, uint32_t ch, float in);

#endif
