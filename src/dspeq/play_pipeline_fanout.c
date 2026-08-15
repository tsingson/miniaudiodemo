#include "play_pipeline_fanout.h"

#include <string.h>

int play_pipeline_fanout_init(play_pipeline_fanout *fanout,
                              uint32_t channels,
                              uint32_t maxFrames)
{
    if (fanout == NULL || channels == 0U || channels > MAX_CHANNELS ||
        maxFrames == 0U || maxFrames > 512U) return -1;
    memset(fanout, 0, sizeof(*fanout));
    fanout->channels = channels;
    fanout->maxFrames = maxFrames;
    for (uint32_t branch = 0U; branch < PLAY_PIPELINE_MAX_BRANCHES; ++branch) {
        play_pipeline_init(&fanout->branches[branch]);
        for (uint32_t channel = 0U; channel < channels; ++channel) {
            fanout->branchPlanar[branch][channel] = fanout->branchStorage[branch][channel];
        }
    }
    return 0;
}

int play_pipeline_fanout_add_branch(play_pipeline_fanout *fanout, float weight)
{
    if (fanout == NULL || fanout->branchCount >= PLAY_PIPELINE_MAX_BRANCHES) return -1;
    fanout->weights[fanout->branchCount] = weight;
    return (int)fanout->branchCount++;
}

play_pipeline *play_pipeline_fanout_branch(play_pipeline_fanout *fanout, uint32_t branch)
{
    if (fanout == NULL || branch >= fanout->branchCount) return NULL;
    return &fanout->branches[branch];
}

int play_pipeline_fanout_process(void *ctx, play_frame_block *block)
{
    play_pipeline_fanout *fanout = ctx;
    if (fanout == NULL || block == NULL || block->layout != PLAY_BUFFER_LAYOUT_PLANAR ||
        block->planar == NULL || block->frameCount == 0U ||
        block->frameCount > fanout->maxFrames || block->channels != fanout->channels) return -1;

    for (uint32_t branch = 0U; branch < fanout->branchCount; ++branch) {
        play_frame_block branchBlock = *block;
        branchBlock.planar = fanout->branchPlanar[branch];
        branchBlock.interleaved = NULL;
        for (uint32_t channel = 0U; channel < fanout->channels; ++channel) {
            memcpy(fanout->branchStorage[branch][channel], block->planar[channel],
                   block->frameCount * sizeof(float));
        }
        if (play_pipeline_run(&fanout->branches[branch], &branchBlock) != 0) return -1;
    }

    for (uint32_t frame = 0U; frame < block->frameCount; ++frame) {
        for (uint32_t channel = 0U; channel < fanout->channels; ++channel) {
            float mixed = 0.0f;
            for (uint32_t branch = 0U; branch < fanout->branchCount; ++branch) {
                mixed += fanout->weights[branch] * fanout->branchStorage[branch][channel][frame];
            }
            block->planar[channel][frame] = mixed;
        }
    }
    return 0;
}
