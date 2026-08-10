#ifndef PLAY_EQ_RUNTIME_H
#define PLAY_EQ_RUNTIME_H

#include <stdint.h>

#include "play_pipeline.h"
#include "play_pipeline_eq_stages.h"

typedef struct play_dsp_state play_dsp_state;

typedef struct
{
    play_eq_stage_profile profile;
    play_linear_eq_stage linearStage;
    play_dynamic_eq_stage dynamicStage;
    play_normal_eq_stage normalStage;
    play_pipeline pipeline;
} play_eq_runtime;

void play_eq_runtime_prepare(play_eq_runtime* runtime, play_dsp_state* state, uint32_t channels);
void play_eq_runtime_process(play_eq_runtime* runtime, play_frame_block* block, int bypassEnabled, uint8_t channelMask);
void play_eq_runtime_commit(play_eq_runtime* runtime, play_dsp_state* state);

#endif