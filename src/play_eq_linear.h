#ifndef PLAY_EQ_LINEAR_H
#define PLAY_EQ_LINEAR_H

#include <stdint.h>

#define PLAY_EQ_LINEAR_MAX_TAPS 33
#define PLAY_EQ_LINEAR_DEFAULT_TAPS 25
#define PLAY_EQ_LINEAR_MAX_CHANNELS 2

typedef struct
{
	uint32_t sampleRate;
	uint32_t channels;
	uint8_t firEnabled;
	uint8_t userEnabled;
	uint8_t taps;
	float coeff[PLAY_EQ_LINEAR_MAX_TAPS];
	float hist[PLAY_EQ_LINEAR_MAX_TAPS][PLAY_EQ_LINEAR_MAX_CHANNELS];
	int writeIndex[PLAY_EQ_LINEAR_MAX_CHANNELS];
	float transientEnv[PLAY_EQ_LINEAR_MAX_CHANNELS];
	float lastDelayed[PLAY_EQ_LINEAR_MAX_CHANNELS];
} play_eq_linear_ctx;

typedef struct play_dsp_state play_dsp_state;

double play_eq_linear_map_gain_db(double sliderGainDb);

void play_eq_linear_ctx_init(play_eq_linear_ctx* ctx, uint32_t sampleRate, uint32_t channels);
void play_eq_linear_ctx_set_config(play_eq_linear_ctx* ctx, int enabled, int taps);
void play_eq_linear_ctx_get_config(const play_eq_linear_ctx* ctx,
								   int* outEnabled,
								   int* outTaps,
								   int* outDelaySamples,
								   float* outDelayMs);
void play_eq_linear_ctx_rebuild(play_eq_linear_ctx* ctx,
								const float* bandFreqs,
								const float* gainsDB,
								const float* qValues,
								const uint8_t* bandModes,
								int bandCount,
								uint8_t linearModeValue);
float play_eq_linear_ctx_process_sample(play_eq_linear_ctx* ctx, uint32_t ch, float in);

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
