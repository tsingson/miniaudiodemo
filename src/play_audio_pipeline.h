#ifndef PLAY_AUDIO_PIPELINE_H
#define PLAY_AUDIO_PIPELINE_H

#include <stdint.h>

#include "play_dsp_common.h"
#include "play_pipeline.h"

#define PLAY_AUDIO_PIPELINE_MAX_FRAMES 512U

typedef int (*play_audio_source_read_fn)(void *ctx,
                                         float *interleaved,
                                         uint32_t capacity,
                                         uint32_t *framesRead);

typedef struct
{
    play_pipeline pipeline;
    play_dsp_state dsp;
    float planarStorage[MAX_CHANNELS][PLAY_AUDIO_PIPELINE_MAX_FRAMES];
    float *planar[MAX_CHANNELS];
    uint32_t sampleRate;
    uint32_t channels;
    uint32_t processedBlocks;
    uint32_t failedBlocks;
} play_audio_pipeline;

int play_audio_pipeline_init(play_audio_pipeline *pipeline, uint32_t sampleRate, uint32_t channels);
int play_audio_pipeline_add_dsp(play_audio_pipeline *pipeline);
int play_audio_pipeline_add_output(play_audio_pipeline *pipeline,
                                   const char *name,
                                   void *ctx,
                                   play_pipeline_stage_fn output);
int play_audio_pipeline_process(play_audio_pipeline *pipeline,
                                const float *interleaved,
                                uint32_t frames);
int play_audio_pipeline_pull(play_audio_pipeline *pipeline,
                             void *sourceCtx,
                             play_audio_source_read_fn readSource);
void play_audio_pipeline_get_stats(const play_audio_pipeline *pipeline,
                                   uint32_t *processedBlocks,
                                   uint32_t *failedBlocks);

#endif