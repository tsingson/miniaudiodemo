#ifndef PLAY_PIPELINE_ASYNC_H
#define PLAY_PIPELINE_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/* Generic async decoupling adapter: bridges a real-time, non-blocking
 * producer (e.g. a USB isochronous receive callback) to a consumer stage
 * that may block for a while (DMA/I2S writes, underrun recovery, ...).
 * The consumer runs on its own worker thread; play_pipeline_async_push()
 * never blocks the caller, it only enqueues and drops on backpressure.
 *
 * This ownership of buffering/timing-decoupling belongs to the pipeline
 * layer, not to any specific input (e.g. UAC2) or output (e.g. PCM5102A)
 * module, so those stay decoupled from each other.
 */
typedef int (*play_pipeline_async_sink_fn)(void *ctx, const float *interleaved, uint32_t frames);

typedef struct {
    struct k_msgq msgq;
    struct k_thread thread;
    k_tid_t tid;
    play_pipeline_async_sink_fn sink;
    void *sink_ctx;
    uint32_t frame_capacity;
    uint32_t channels;
    uint32_t queue_depth;
    atomic_t delivered;
    atomic_t dropped;
} play_pipeline_async;

/* All buffers/stack are caller-owned (static storage) so this adapter makes
 * no dynamic allocations:
 *   - stack/stack_size: worker thread stack, e.g. K_THREAD_STACK_DEFINE().
 *   - msgq_buffer: at least queue_depth * frame_capacity * channels * sizeof(float) bytes.
 *   - scratch_block: exactly frame_capacity * channels floats, used by the
 *     worker thread to receive one message before handing it to sink.
 */
int play_pipeline_async_init(play_pipeline_async *async,
                              k_thread_stack_t *stack, size_t stack_size,
                              char *msgq_buffer, uint32_t queue_depth,
                              float *scratch_block,
                              uint32_t frame_capacity, uint32_t channels,
                              int priority,
                              play_pipeline_async_sink_fn sink, void *sink_ctx);

/* Matches play_pipeline_async_sink_fn so it can be registered directly as
 * another module's push-style sink (e.g. usb_uac2_set_audio_sink()).
 * ctx must be the play_pipeline_async* returned by _init(). */
int play_pipeline_async_push(void *ctx, const float *interleaved, uint32_t frames);

uint32_t play_pipeline_async_get_delivered(const play_pipeline_async *async);
uint32_t play_pipeline_async_get_dropped(const play_pipeline_async *async);

/* Pending-queue fill level in per-mille (0-1000). Generic proxy signal for
 * downstream buffer occupancy, e.g. usable as a UAC2 feedback source without
 * UAC2 needing to know anything about the concrete output device. */
uint32_t play_pipeline_async_fill_permille(void *ctx);

#endif
