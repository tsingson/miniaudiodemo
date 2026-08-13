#include "play_pipeline_mix.h"

#include <string.h>

void play_pipeline_mix_init(play_pipeline_mix *mix)
{
    if (mix != NULL) memset(mix, 0, sizeof(*mix));
}

int play_pipeline_mix_set_input(play_pipeline_mix *mix,
                                uint32_t index,
                                const play_frame_block *input,
                                float weight)
{
    if (mix == NULL || index >= PLAY_PIPELINE_MAX_MIX_INPUTS || input == NULL) return -1;
    mix->inputs[index] = input;
    mix->weights[index] = weight;
    if (mix->inputCount <= index) mix->inputCount = index + 1U;
    return 0;
}

int play_pipeline_mix_process(void *ctx, play_frame_block *output)
{
    play_pipeline_mix *mix = ctx;
    if (mix == NULL || output == NULL || output->layout != PLAY_BUFFER_LAYOUT_PLANAR ||
        output->planar == NULL || output->frameCount == 0U) return -1;

    for (uint32_t inputIndex = 0U; inputIndex < mix->inputCount; ++inputIndex) {
        const play_frame_block *input = mix->inputs[inputIndex];
        if (input == NULL || input->layout != PLAY_BUFFER_LAYOUT_PLANAR ||
            input->planar == NULL || input->frameCount != output->frameCount ||
            input->channels != output->channels || input->sampleRate != output->sampleRate) return -1;
    }

    for (uint32_t frame = 0U; frame < output->frameCount; ++frame) {
        for (uint32_t channel = 0U; channel < output->channels; ++channel) {
            float value = 0.0f;
            for (uint32_t inputIndex = 0U; inputIndex < mix->inputCount; ++inputIndex) {
                value += mix->inputs[inputIndex]->planar[channel][frame] * mix->weights[inputIndex];
            }
            output->planar[channel][frame] = value;
        }
    }
    return 0;
}
