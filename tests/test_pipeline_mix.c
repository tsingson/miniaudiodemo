#include "play_pipeline_mix.h"

#include <math.h>
#include <stdio.h>

int main(void)
{
    float sourceA[2] = {1.0f, -1.0f};
    float sourceB[2] = {0.5f, 0.25f};
    float output[2] = {0.0f, 0.0f};
    float *aPlanar[1] = {sourceA};
    float *bPlanar[1] = {sourceB};
    float *outPlanar[1] = {output};
    play_frame_block a = {.planar = aPlanar, .layout = PLAY_BUFFER_LAYOUT_PLANAR,
                           .frameCount = 2U, .channels = 1U, .sampleRate = 48000U};
    play_frame_block b = a;
    play_frame_block out = a;
    play_pipeline_mix mix;

    b.planar = bPlanar;
    out.planar = outPlanar;
    play_pipeline_mix_init(&mix);
    if (play_pipeline_mix_set_input(&mix, 0U, &a, 0.5f) != 0 ||
        play_pipeline_mix_set_input(&mix, 1U, &b, 2.0f) != 0 ||
        play_pipeline_mix_process(&mix, &out) != 0) return 1;
    if (fabsf(output[0] - 1.5f) > 1e-6f || fabsf(output[1] - 0.0f) > 1e-6f) return 1;
    puts("PASS multiple-input weighted mix node");
    return 0;
}
