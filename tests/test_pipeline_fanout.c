#include "play_pipeline_fanout.h"

#include <math.h>
#include <stdio.h>

struct gain_ctx { float gain; };

static int gain_stage(void *ctx, play_frame_block *block)
{
    struct gain_ctx *gain = ctx;
    for (uint32_t frame = 0U; frame < block->frameCount; ++frame) {
        for (uint32_t channel = 0U; channel < block->channels; ++channel) {
            block->planar[channel][frame] *= gain->gain;
        }
    }
    return 0;
}

int main(void)
{
    play_pipeline_fanout fanout;
    struct gain_ctx gainA = {2.0f};
    struct gain_ctx gainB = {0.5f};
    float left[2] = {1.0f, -1.0f};
    float right[2] = {0.5f, -0.5f};
    float *planar[2] = {left, right};
    play_frame_block block = {
        .planar = planar,
        .layout = PLAY_BUFFER_LAYOUT_PLANAR,
        .frameCount = 2U,
        .channels = 2U,
        .channelMask = 0U,
        .sampleRate = 48000U,
    };

    if (play_pipeline_fanout_init(&fanout, 2U, 2U) != 0) return 1;
    if (play_pipeline_fanout_add_branch(&fanout, 0.5f) < 0 ||
        play_pipeline_fanout_add_branch(&fanout, 0.5f) < 0) return 1;
    if (play_pipeline_add_stage(play_pipeline_fanout_branch(&fanout, 0), "gain-a", 1,
                                &gainA, gain_stage) < 0 ||
        play_pipeline_add_stage(play_pipeline_fanout_branch(&fanout, 1), "gain-b", 1,
                                &gainB, gain_stage) < 0) return 1;

    if (play_pipeline_fanout_process(&fanout, &block) != 0) return 1;
    if (fabsf(left[0] - 1.25f) > 1e-6f || fabsf(right[0] - 0.625f) > 1e-6f ||
        fabsf(left[1] + 1.25f) > 1e-6f) return 1;

    puts("PASS fanout route branch processing and weighted mix");
    return 0;
}
