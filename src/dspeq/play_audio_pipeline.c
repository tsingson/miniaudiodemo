#include "play_audio_pipeline.h"

#include <string.h>

static int dsp_stage(void *ctx, play_frame_block *block)
{
    play_audio_pipeline *pipeline = (play_audio_pipeline *)ctx;
    if (block->layout != PLAY_BUFFER_LAYOUT_PLANAR) {
        return -1;
    }
    play_dsp_process_planar(&pipeline->dsp, pipeline->planar,
                            block->frameCount, block->channels);
    return 0;
}

int play_audio_pipeline_init(play_audio_pipeline *pipeline, uint32_t sampleRate, uint32_t channels)
{
    if (pipeline == NULL || channels == 0U || channels > MAX_CHANNELS) {
        return -1;
    }
    memset(pipeline, 0, sizeof(*pipeline));
    if (play_dsp_init(&pipeline->dsp, sampleRate, channels) != 0) {
        return -1;
    }
    pipeline->sampleRate = sampleRate;
    pipeline->channels = channels;
    for (uint32_t ch = 0U; ch < channels; ++ch) {
        pipeline->planar[ch] = pipeline->planarStorage[ch];
    }
    play_pipeline_init(&pipeline->pipeline);
    return 0;
}

int play_audio_pipeline_add_dsp(play_audio_pipeline *pipeline)
{
    if (pipeline == NULL) return -1;
    return play_pipeline_add_stage(&pipeline->pipeline, "dsp", 1, pipeline, dsp_stage) >= 0 ? 0 : -1;
}

int play_audio_pipeline_add_output(play_audio_pipeline *pipeline,
                                   const char *name,
                                   void *ctx,
                                   play_pipeline_stage_fn output)
{
    if (pipeline == NULL || output == NULL) return -1;
    return play_pipeline_add_stage(&pipeline->pipeline, name, 1, ctx, output) >= 0 ? 0 : -1;
}

int play_audio_pipeline_process(play_audio_pipeline *pipeline,
                                const float *interleaved,
                                uint32_t frames)
{
    play_frame_block block;
    if (pipeline == NULL || interleaved == NULL || frames == 0U ||
        frames > PLAY_AUDIO_PIPELINE_MAX_FRAMES || pipeline->channels == 0U ||
        pipeline->channels > MAX_CHANNELS || pipeline->sampleRate == 0U) {
        if (pipeline != NULL) pipeline->failedBlocks++;
        return -1;
    }
    for (uint32_t frame = 0U; frame < frames; ++frame) {
        for (uint32_t ch = 0U; ch < pipeline->channels; ++ch) {
            pipeline->planarStorage[ch][frame] = interleaved[frame * pipeline->channels + ch];
        }
    }
    block.interleaved = NULL;
    block.planar = pipeline->planar;
    block.layout = PLAY_BUFFER_LAYOUT_PLANAR;
    block.frameCount = frames;
    block.channels = pipeline->channels;
    block.channelMask = 0U;
    block.sampleRate = pipeline->sampleRate;
    int ret = play_pipeline_run(&pipeline->pipeline, &block);
    if (ret == 0) pipeline->processedBlocks++;
    else pipeline->failedBlocks++;
    return ret;
}

void play_audio_pipeline_get_stats(const play_audio_pipeline *pipeline,
                                   uint32_t *processedBlocks,
                                   uint32_t *failedBlocks)
{
    if (processedBlocks != NULL) *processedBlocks = pipeline != NULL ? pipeline->processedBlocks : 0U;
    if (failedBlocks != NULL) *failedBlocks = pipeline != NULL ? pipeline->failedBlocks : 0U;
}

int play_audio_pipeline_pull(play_audio_pipeline *pipeline,
                             void *sourceCtx,
                             play_audio_source_read_fn readSource)
{
    float block[PLAY_AUDIO_PIPELINE_MAX_FRAMES * MAX_CHANNELS];
    uint32_t frames = 0U;
    int ret;

    if (pipeline == NULL || readSource == NULL) return -1;
    ret = readSource(sourceCtx, block, PLAY_AUDIO_PIPELINE_MAX_FRAMES, &frames);
    if (ret != 0 || frames == 0U) return ret;
    return play_audio_pipeline_process(pipeline, block, frames);
}