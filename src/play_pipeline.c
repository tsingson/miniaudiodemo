#include "play_pipeline.h"

#include <stddef.h>

#include "miniaudio.h"

void play_pipeline_init(play_pipeline* pipeline)
{
    if (pipeline == NULL)
    {
        return;
    }

    pipeline->stageCount = 0;
    pipeline->bypass = 0;
}

int play_pipeline_add_stage(play_pipeline* pipeline,
                            const char* name,
                            int enabled,
                            void* ctx,
                            play_pipeline_stage_fn process)
{
    if (pipeline == NULL || process == NULL)
    {
        return -1;
    }

    if (pipeline->stageCount >= PLAY_PIPELINE_MAX_STAGES)
    {
        return -1;
    }

    const int idx = pipeline->stageCount;
    pipeline->stages[idx].name = name;
    pipeline->stages[idx].enabled = enabled ? 1 : 0;
    pipeline->stages[idx].ctx = ctx;
    pipeline->stages[idx].process = process;
    pipeline->stageCount += 1;

    return idx;
}

int play_pipeline_set_stage_enabled(play_pipeline* pipeline, int index, int enabled)
{
    if (pipeline == NULL || index < 0 || index >= pipeline->stageCount)
    {
        return -1;
    }

    pipeline->stages[index].enabled = enabled ? 1 : 0;
    return 0;
}

void play_pipeline_set_bypass(play_pipeline* pipeline, int bypass)
{
    if (pipeline == NULL)
    {
        return;
    }

    pipeline->bypass = bypass ? 1 : 0;
}

int play_pipeline_run(play_pipeline* pipeline, play_frame_block* block)
{
    int hasData;

    if (pipeline == NULL || block == NULL)
    {
        return -1;
    }

    hasData = (block->layout == PLAY_BUFFER_LAYOUT_PLANAR) ? (block->planar != NULL) : (block->interleaved != NULL);
    if (!hasData)
    {
        return -1;
    }

    if (pipeline->bypass)
    {
        return 0;
    }

    for (int i = 0; i < pipeline->stageCount; ++i)
    {
        if (!pipeline->stages[i].enabled)
        {
            continue;
        }

        if (pipeline->stages[i].process(pipeline->stages[i].ctx, block) != 0)
        {
            return -1;
        }
    }

    return 0;
}
