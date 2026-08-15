#include "uac2_audio_stream.h"

#include <errno.h>
#include <string.h>

#include "play_pipeline_async.h"
#include "usb_uac2_device.h"

#define UAC2_AUDIO_QUEUE_DEPTH 3U
#define UAC2_AUDIO_WORKER_PRIORITY 10
#define UAC2_AUDIO_WORKER_STACK_SIZE 1536U

#ifndef MINIAUDIO_UAC2_NULL_SINK
static K_THREAD_STACK_DEFINE(audio_worker_stack, UAC2_AUDIO_WORKER_STACK_SIZE);
static char audio_queue_buffer[UAC2_AUDIO_QUEUE_DEPTH *
                               UAC2_AUDIO_BLOCK_FRAMES *
                               UAC2_AUDIO_CHANNELS * sizeof(float)];
static float audio_scratch[UAC2_AUDIO_BLOCK_FRAMES * UAC2_AUDIO_CHANNELS];
static play_pipeline_async audio_async;
#endif

int uac2_audio_stream_init(const struct uac2_audio_stream_config *config)
{
    if (config == NULL) {
        return -EINVAL;
    }

#ifndef MINIAUDIO_UAC2_NULL_SINK
    int ret;

    if (config->process == NULL) {
        return -EINVAL;
    }
    ret = play_pipeline_async_init(&audio_async,
                                   audio_worker_stack,
                                   K_THREAD_STACK_SIZEOF(audio_worker_stack),
                                   audio_queue_buffer,
                                   UAC2_AUDIO_QUEUE_DEPTH,
                                   audio_scratch,
                                   UAC2_AUDIO_BLOCK_FRAMES,
                                   UAC2_AUDIO_CHANNELS,
                                   UAC2_AUDIO_WORKER_PRIORITY,
                                   config->process,
                                   config->process_ctx);
    if (ret != 0) {
        return ret;
    }
    usb_uac2_set_audio_sink(play_pipeline_async_push, &audio_async);
    if (config->feedback_fill != NULL) {
        usb_uac2_set_feedback_source(config->feedback_fill,
                                     config->feedback_ctx);
    }
#else
    usb_uac2_set_audio_sink(NULL, NULL);
#endif

    return usb_uac2_device_init(UAC2_AUDIO_SAMPLE_RATE,
                                UAC2_AUDIO_CHANNELS,
                                UAC2_AUDIO_BITS_PER_SAMPLE);
}

bool uac2_audio_stream_active(void)
{
    return usb_uac2_stream_active();
}

bool uac2_audio_stream_first_packet_seen(void)
{
    return usb_uac2_first_packet_seen();
}

void uac2_audio_stream_get_stats(struct uac2_audio_stream_stats *stats)
{
    if (stats == NULL) {
        return;
    }

    memset(stats, 0, sizeof(*stats));
    usb_uac2_get_stats(&stats->packets,
                       &stats->bytes,
                       &stats->frames,
                       &stats->buffer_requests,
                       &stats->buffer_rejects,
                       &stats->zero_packets,
                       &stats->invalid_packets,
                       &stats->odd_byte_packets,
                       &stats->checksum);
#ifndef MINIAUDIO_UAC2_NULL_SINK
    stats->delivered = play_pipeline_async_get_delivered(&audio_async);
#endif
    stats->dropped = usb_uac2_get_audio_out_dropped();
    stats->feedback_adjust = usb_uac2_get_feedback_adjust();
    stats->feedback_calls = usb_uac2_get_feedback_call_count();
}

void uac2_audio_stream_sample_feedback_fill_range(int32_t *min_fill,
                                                  int32_t *max_fill)
{
    usb_uac2_sample_feedback_fill_range(min_fill, max_fill);
}
