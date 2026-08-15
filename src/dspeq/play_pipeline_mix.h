#ifndef PLAY_PIPELINE_MIX_H
#define PLAY_PIPELINE_MIX_H

#include <stdint.h>

#include "play_dsp_defs.h"
#include "play_pipeline.h"

#define PLAY_PIPELINE_MAX_MIX_INPUTS 4U

typedef struct
{
    const play_frame_block *inputs[PLAY_PIPELINE_MAX_MIX_INPUTS];
    float weights[PLAY_PIPELINE_MAX_MIX_INPUTS];
    uint32_t inputCount;
} play_pipeline_mix;

void play_pipeline_mix_init(play_pipeline_mix *mix);
int play_pipeline_mix_set_input(play_pipeline_mix *mix,
                                uint32_t index,
                                const play_frame_block *input,
                                float weight);
int play_pipeline_mix_process(void *ctx, play_frame_block *output);

#endif
