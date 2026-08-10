#ifndef PLAY_PIPELINE_EQ_STAGES_H
#define PLAY_PIPELINE_EQ_STAGES_H

#include <stdint.h>

#include "play_dsp_common.h"
#include "play_eq_dynamic.h"
#include "play_eq_linear.h"
#include "play_pipeline.h"

typedef struct
{
    uint32_t sampleRate;
    uint32_t channels;
    int bandCount;
    float freqs[EQ_BANDS];
    float gainsDB[EQ_BANDS];
    float qValues[EQ_BANDS];
    uint8_t modes[EQ_BANDS];
} play_eq_stage_profile;

typedef struct
{
    play_eq_linear_ctx linear;
    play_eq_stage_profile profile;
    uint8_t linearModeValue;
} play_linear_eq_stage;

typedef struct
{
    uint32_t sampleRate;
    uint32_t channels;
    int bandCount;
    float gainsDB[EQ_BANDS];
    uint8_t modes[EQ_BANDS];
    uint8_t useSVF[EQ_BANDS];

    double b0[EQ_BANDS];
    double b1[EQ_BANDS];
    double b2[EQ_BANDS];
    double a1[EQ_BANDS];
    double a2[EQ_BANDS];

    double svfG[EQ_BANDS];
    double svfK[EQ_BANDS];
    double svfA[EQ_BANDS];
    double svfH[EQ_BANDS];

    double svfIc1eq[EQ_BANDS][MAX_CHANNELS];
    double svfIc2eq[EQ_BANDS][MAX_CHANNELS];

    double x1[EQ_BANDS][MAX_CHANNELS];
    double x2[EQ_BANDS][MAX_CHANNELS];
    double y1[EQ_BANDS][MAX_CHANNELS];
    double y2[EQ_BANDS][MAX_CHANNELS];

    float dynamicEnv[EQ_BANDS][MAX_CHANNELS];
    float dynamicReductionDB[EQ_BANDS];
    play_eq_dynamic_params params;

    uint8_t dynamicModeValue;
    uint8_t linearModeValue;
} play_dynamic_eq_stage;

typedef struct
{
    uint32_t sampleRate;
    uint32_t channels;
    int bandCount;
    uint8_t modes[EQ_BANDS];
    uint8_t useSVF[EQ_BANDS];

    double b0[EQ_BANDS];
    double b1[EQ_BANDS];
    double b2[EQ_BANDS];
    double a1[EQ_BANDS];
    double a2[EQ_BANDS];

    double svfG[EQ_BANDS];
    double svfK[EQ_BANDS];
    double svfA[EQ_BANDS];
    double svfH[EQ_BANDS];

    double svfIc1eq[EQ_BANDS][MAX_CHANNELS];
    double svfIc2eq[EQ_BANDS][MAX_CHANNELS];

    double x1[EQ_BANDS][MAX_CHANNELS];
    double x2[EQ_BANDS][MAX_CHANNELS];
    double y1[EQ_BANDS][MAX_CHANNELS];
    double y2[EQ_BANDS][MAX_CHANNELS];

    uint8_t normalModeValue;
    uint8_t linearModeValue;
} play_normal_eq_stage;

typedef struct
{
    play_dsp_state* state;
    uint8_t position;
} play_crossfeed_stage;

typedef struct
{
    play_dsp_state* state;
    uint8_t position;
} play_mbdyn_stage;

typedef struct
{
    float monoIn;
    float monoEqIn;
    float monoEqOut;
    float monoOut;
} play_observer_metrics;

typedef enum
{
    PLAY_OBSERVER_INPUT_RAW = 0,
    PLAY_OBSERVER_INPUT_EQ = 1,
    PLAY_OBSERVER_OUTPUT_EQ = 2,
    PLAY_OBSERVER_OUTPUT_FINAL = 3
} play_observer_stage_kind;

typedef struct
{
    play_dsp_state* state;
    play_observer_metrics* metrics;
    play_observer_stage_kind kind;
    void (*onWindowComplete)(play_dsp_state* state);
} play_observer_stage;

void play_eq_stage_profile_init(play_eq_stage_profile* profile,
                                uint32_t sampleRate,
                                uint32_t channels,
                                const float* freqs,
                                const float* gains,
                                const float* qs,
                                const uint8_t* modes,
                                int bandCount);

void play_linear_eq_stage_init(play_linear_eq_stage* stage,
                               const play_eq_stage_profile* profile,
                               int enabled,
                               int taps,
                               uint8_t linearModeValue);
void play_linear_eq_stage_rebuild(play_linear_eq_stage* stage);

void play_dynamic_eq_stage_init(play_dynamic_eq_stage* stage,
                                uint32_t sampleRate,
                                uint32_t channels,
                                const float* freqs,
                                const float* gains,
                                const float* qs,
                                const uint8_t* modes,
                                int bandCount,
                                uint8_t linearModeValue,
                                uint8_t dynamicModeValue);
void play_dynamic_eq_stage_set_params(play_dynamic_eq_stage* stage,
                                      float attack,
                                      float release,
                                      float threshold,
                                      float maxReductionDB,
                                      float strengthDB);

void play_normal_eq_stage_init(play_normal_eq_stage* stage,
                               uint32_t sampleRate,
                               uint32_t channels,
                               const float* freqs,
                               const float* gains,
                               const float* qs,
                               const uint8_t* modes,
                               int bandCount,
                               uint8_t linearModeValue,
                               uint8_t normalModeValue);

void play_crossfeed_stage_init(play_crossfeed_stage* stage, play_dsp_state* state, uint8_t position);
void play_mbdyn_stage_init(play_mbdyn_stage* stage, play_dsp_state* state, uint8_t position);
void play_observer_stage_init(play_observer_stage* stage,
                              play_dsp_state* state,
                              play_observer_metrics* metrics,
                              play_observer_stage_kind kind,
                              void (*onWindowComplete)(play_dsp_state* state));

int play_linear_eq_stage_process(void* ctx, play_frame_block* block);
int play_dynamic_eq_stage_process(void* ctx, play_frame_block* block);
int play_normal_eq_stage_process(void* ctx, play_frame_block* block);
int play_crossfeed_stage_process(void* ctx, play_frame_block* block);
int play_mbdyn_stage_process(void* ctx, play_frame_block* block);
int play_observer_stage_process(void* ctx, play_frame_block* block);

#endif