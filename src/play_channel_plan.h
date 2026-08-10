#ifndef PLAY_CHANNEL_PLAN_H
#define PLAY_CHANNEL_PLAN_H

#include <stdint.h>

#define PLAY_CHANNEL_PLAN_MAX_CHANNELS 8

typedef enum
{
    PLAY_CHANNEL_LAYOUT_UNKNOWN = 0,
    PLAY_CHANNEL_LAYOUT_STEREO = 2,
    PLAY_CHANNEL_LAYOUT_5_1 = 6,
    PLAY_CHANNEL_LAYOUT_7_1 = 8
} play_channel_layout;

typedef struct
{
    uint32_t inChannels;
    uint32_t outChannels;
    float matrix[PLAY_CHANNEL_PLAN_MAX_CHANNELS][PLAY_CHANNEL_PLAN_MAX_CHANNELS];
} play_channel_plan;

void play_channel_plan_init_identity(play_channel_plan* plan, uint32_t channels);
int play_channel_plan_prepare_stereo_to_5_1(play_channel_plan* plan);
int play_channel_plan_set_matrix(play_channel_plan* plan,
                                 uint32_t inChannels,
                                 uint32_t outChannels,
                                 const float* matrixRowMajor,
                                 uint32_t matrixCount);
int play_channel_plan_apply_planar(const play_channel_plan* plan,
                                   float* const* inPlanar,
                                   float* const* outPlanar,
                                   uint32_t frameCount);

#endif