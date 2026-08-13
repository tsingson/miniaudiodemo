#include "play_audio_pipeline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_FRAMES 128U
#define TEST_CHANNELS 2U
#define TEST_RATE 48000U

static void fill_input(float *interleaved)
{
    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        float sample = 0.25f * sinf(2.0f * (float)PLAY_DSP_PI * 440.0f *
                                    (float)frame / (float)TEST_RATE);
        interleaved[frame * TEST_CHANNELS] = sample;
        interleaved[frame * TEST_CHANNELS + 1U] = sample * 0.8f;
    }
}

int main(void)
{
    float input[TEST_FRAMES * TEST_CHANNELS];
    float mac_planar_storage[TEST_CHANNELS][TEST_FRAMES];
    float *mac_planar[TEST_CHANNELS] = {mac_planar_storage[0], mac_planar_storage[1]};
    play_dsp_state mac_dsp;
    play_audio_pipeline stm_pipeline;
    float max_difference = 0.0f;

    fill_input(input);
    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        mac_planar_storage[0][frame] = input[frame * TEST_CHANNELS];
        mac_planar_storage[1][frame] = input[frame * TEST_CHANNELS + 1U];
    }

    if (play_dsp_init(&mac_dsp, TEST_RATE, TEST_CHANNELS) != 0 ||
        play_audio_pipeline_init(&stm_pipeline, TEST_RATE, TEST_CHANNELS) != 0 ||
        play_audio_pipeline_add_dsp(&stm_pipeline) != 0) {
        printf("FAIL pipeline initialization\n");
        return 1;
    }

    play_dsp_process_planar(&mac_dsp, mac_planar, TEST_FRAMES, TEST_CHANNELS);

    if (play_audio_pipeline_process(&stm_pipeline, input, TEST_FRAMES) != 0) {
        printf("FAIL pipeline processing\n");
        return 1;
    }

    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        float left_difference = fabsf(mac_planar_storage[0][frame] -
                          stm_pipeline.planarStorage[0][frame]);
        float right_difference = fabsf(mac_planar_storage[1][frame] -
                           stm_pipeline.planarStorage[1][frame]);
        if (left_difference > max_difference) max_difference = left_difference;
        if (right_difference > max_difference) max_difference = right_difference;
    }

    if (max_difference > 1e-6f) {
        printf("FAIL macOS-vs-STM32 pipeline max_difference=%.9g\n", max_difference);
        return 1;
    }

    printf("PASS macOS-vs-STM32 pipeline max_difference=%.9g\n", max_difference);
    return 0;
}
