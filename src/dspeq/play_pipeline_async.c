#include "play_pipeline_async.h"

#include <errno.h>
#include <string.h>

static void async_thread_fn(void *p1, void *p2, void *p3)
{
    play_pipeline_async *async = (play_pipeline_async *)p1;
    float *block = (float *)p2;

    ARG_UNUSED(p3);
    while (1) {
        if (k_msgq_get(&async->msgq, block, K_FOREVER) == 0 && async->sink != NULL) {
            if (async->sink(async->sink_ctx, block, async->frame_capacity) == 0) {
                atomic_inc(&async->delivered);
            }
        }
    }
}

int play_pipeline_async_init(play_pipeline_async *async,
                              k_thread_stack_t *stack, size_t stack_size,
                              char *msgq_buffer, uint32_t queue_depth,
                              float *scratch_block,
                              uint32_t frame_capacity, uint32_t channels,
                              int priority,
                              play_pipeline_async_sink_fn sink, void *sink_ctx)
{
    if (async == NULL || stack == NULL || msgq_buffer == NULL || scratch_block == NULL ||
        frame_capacity == 0U || channels == 0U || queue_depth == 0U || sink == NULL) {
        return -EINVAL;
    }

    memset(async, 0, sizeof(*async));
    async->sink = sink;
    async->sink_ctx = sink_ctx;
    async->frame_capacity = frame_capacity;
    async->channels = channels;
    async->queue_depth = queue_depth;

    k_msgq_init(&async->msgq, msgq_buffer,
                (uint32_t)(frame_capacity * channels * sizeof(float)), queue_depth);
    async->tid = k_thread_create(&async->thread, stack, stack_size, async_thread_fn,
                                  async, scratch_block, NULL, priority, 0, K_NO_WAIT);
    return 0;
}

int play_pipeline_async_push(void *ctx, const float *interleaved, uint32_t frames)
{
    play_pipeline_async *async = (play_pipeline_async *)ctx;

    if (async == NULL || interleaved == NULL || frames != async->frame_capacity) {
        return -EINVAL;
    }
    if (k_msgq_put(&async->msgq, interleaved, K_NO_WAIT) != 0) {
        atomic_inc(&async->dropped);
        return -ENOMEM;
    }
    return 0;
}

uint32_t play_pipeline_async_get_delivered(const play_pipeline_async *async)
{
    return async != NULL ? (uint32_t)atomic_get(&async->delivered) : 0U;
}

uint32_t play_pipeline_async_get_dropped(const play_pipeline_async *async)
{
    return async != NULL ? (uint32_t)atomic_get(&async->dropped) : 0U;
}

uint32_t play_pipeline_async_fill_permille(void *ctx)
{
    const play_pipeline_async *async = (const play_pipeline_async *)ctx;
    uint32_t used;

    if (async == NULL || async->queue_depth == 0U) {
        return 0U;
    }
    used = (uint32_t)k_msgq_num_used_get((struct k_msgq *)&async->msgq);
    return (used * 1000U) / async->queue_depth;
}
