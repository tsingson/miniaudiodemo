#ifndef PLAY_PIPELINE_H
#define PLAY_PIPELINE_H

#include <stdint.h>

#define PLAY_PIPELINE_MAX_STAGES 16

typedef enum
{
    PLAY_BUFFER_LAYOUT_INTERLEAVED = 0,
    PLAY_BUFFER_LAYOUT_PLANAR = 1
} play_buffer_layout;

typedef struct
{
    float* interleaved;
    float* const* planar;
    play_buffer_layout layout;
    uint32_t frameCount;
    uint32_t channels;
    uint8_t channelMask;
    uint32_t sampleRate;
} play_frame_block;

typedef int (*play_pipeline_stage_fn)(void* ctx, play_frame_block* block);

typedef struct
{
    const char* name;
    int enabled;
    void* ctx;
    play_pipeline_stage_fn process;
} play_pipeline_stage;

typedef struct
{
    play_pipeline_stage stages[PLAY_PIPELINE_MAX_STAGES];
    int stageCount;
    int bypass;
} play_pipeline;

void play_pipeline_init(play_pipeline* pipeline);
int play_pipeline_add_stage(play_pipeline* pipeline,
                            const char* name,
                            int enabled,
                            void* ctx,
                            play_pipeline_stage_fn process);
int play_pipeline_set_stage_enabled(play_pipeline* pipeline, int index, int enabled);
void play_pipeline_set_bypass(play_pipeline* pipeline, int bypass);
int play_pipeline_run(play_pipeline* pipeline, play_frame_block* block);

#endif