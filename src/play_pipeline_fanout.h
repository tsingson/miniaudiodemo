#ifndef PLAY_PIPELINE_FANOUT_H
#define PLAY_PIPELINE_FANOUT_H

#include <stdint.h>

#include "play_dsp_defs.h"
#include "play_pipeline.h"

#define PLAY_PIPELINE_MAX_BRANCHES 4U

/* A fanout node owns branch scratch buffers and mixes branch results back into
 * the incoming block. Branch pipelines are ordinary play_pipeline objects. */
typedef struct
{
    play_pipeline branches[PLAY_PIPELINE_MAX_BRANCHES];
    float branchStorage[PLAY_PIPELINE_MAX_BRANCHES][MAX_CHANNELS][512U];
    float *branchPlanar[PLAY_PIPELINE_MAX_BRANCHES][MAX_CHANNELS];
    float weights[PLAY_PIPELINE_MAX_BRANCHES];
    uint32_t branchCount;
    uint32_t channels;
    uint32_t maxFrames;
} play_pipeline_fanout;

int play_pipeline_fanout_init(play_pipeline_fanout *fanout,
                              uint32_t channels,
                              uint32_t maxFrames);
int play_pipeline_fanout_add_branch(play_pipeline_fanout *fanout, float weight);
play_pipeline *play_pipeline_fanout_branch(play_pipeline_fanout *fanout, uint32_t branch);
int play_pipeline_fanout_process(void *ctx, play_frame_block *block);

#endif
