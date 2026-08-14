#include <stdbool.h>
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_3_wav_data.h"
#include "play_audio_pipeline.h"
#include "play_pipeline_async.h"
#include "pcm5102a_audio.h"
#include "usb_uac2_device.h"

LOG_MODULE_REGISTER(main_pcm5102_st7735s_uac2, LOG_LEVEL_INF);

#define UAC2_SAMPLE_RATE 48000U
#define UAC2_CHANNELS 2U
#define LOCAL_BLOCK_FRAMES 128U
#ifndef MINIAUDIO_UAC2_PID
#define MINIAUDIO_UAC2_PID 0x0102
#endif

/* Async decoupling between the UAC2 receive path and the (potentially
 * blocking) PCM5102A output stage. Owned here, in the composition root:
 * neither the uac2 nor the pcm5102a module needs to know about the other.
 */
#define AUDIO_OUT_QUEUE_DEPTH 3
#ifndef MINIAUDIO_UAC2_NULL_SINK
static K_THREAD_STACK_DEFINE(audio_out_stack, 1536);
static char audio_out_msgq_buffer[AUDIO_OUT_QUEUE_DEPTH *
                                  LOCAL_BLOCK_FRAMES * UAC2_CHANNELS * sizeof(float)];
static float audio_out_scratch[LOCAL_BLOCK_FRAMES * UAC2_CHANNELS];
static play_pipeline_async audio_out_async;
#endif

static int pipeline_sink(void *ctx, const float *frames, uint32_t count)
{
    int ret = play_audio_pipeline_process((play_audio_pipeline *)ctx, frames, count);
    if (ret != 0) {
        LOG_WRN_ONCE("pipeline_sink first failure: ret=%d count=%u", ret, count);
    }
    return ret;
}

#ifndef MINIAUDIO_UAC2_NULL_SINK
/* Feedback needs to track the buffer that actually absorbs USB-vs-DAC clock
 * drift over multi-second timescales: the PCM5102A I2S TX slab (~43ms deep).
 * The async decoupling queue in front of it is too shallow (~8ms) and its
 * occupancy is dominated by scheduling jitter, not the slow drift trend, so
 * it is not a suitable feedback signal on its own.
 */
static uint32_t pcm5102a_fill_permille(void *ctx)
{
    uint32_t used = pcm5102a_audio_get_slab_used();
    uint32_t capacity = pcm5102a_audio_get_slab_capacity();

    ARG_UNUSED(ctx);
    if (capacity == 0U) {
        return 0U;
    }
    return (used * 1000U) / capacity;
}
#endif

struct local_wav_view {
    const int16_t *data;
    uint32_t frames;
    uint16_t channels;
    uint32_t sample_rate;
};

static void key_log(const char *message)
{
    LOG_INF("%s", message);
}

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static int parse_local_wav(struct local_wav_view *wav)
{
    const unsigned char *data = g_audio_3_wav;
    uint32_t pos = 12U;
    uint16_t format = 0U;
    uint16_t channels = 0U;
    uint16_t bits = 0U;
    uint32_t rate = 0U;
    const int16_t *pcm = NULL;
    uint32_t bytes = 0U;

    if (g_audio_3_wav_len < 44U || memcmp(data, "RIFF", 4) != 0 ||
        memcmp(data + 8U, "WAVE", 4) != 0) {
        return -EINVAL;
    }
    while (pos + 8U <= g_audio_3_wav_len) {
        uint32_t size = read_le32(data + pos + 4U);
        uint32_t payload = pos + 8U;
        if (payload + size > g_audio_3_wav_len) {
            return -EINVAL;
        }
        if (memcmp(data + pos, "fmt ", 4) == 0 && size >= 16U) {
            format = read_le16(data + payload);
            channels = read_le16(data + payload + 2U);
            rate = read_le32(data + payload + 4U);
            bits = read_le16(data + payload + 14U);
        } else if (memcmp(data + pos, "data", 4) == 0) {
            pcm = (const int16_t *)(data + payload);
            bytes = size;
        }
        pos = payload + size + (size & 1U);
    }
    if (format != 1U || (channels != 1U && channels != 2U) || bits != 16U || pcm == NULL) {
        return -EINVAL;
    }
    wav->data = pcm;
    wav->frames = bytes / ((uint32_t)channels * sizeof(int16_t));
    wav->channels = channels;
    wav->sample_rate = rate;
    return wav->frames > 0U ? 0 : -EINVAL;
}

static void fill_local_block(float *out, const struct local_wav_view *wav, uint64_t *phase)
{
    uint64_t wrap = (uint64_t)wav->frames << 32U;
    uint64_t step = ((uint64_t)wav->sample_rate << 32U) / UAC2_SAMPLE_RATE;
    for (uint32_t i = 0U; i < LOCAL_BLOCK_FRAMES; ++i) {
        uint32_t index;
        while (*phase >= wrap) {
            *phase -= wrap;
        }
        index = (uint32_t)(*phase >> 32U);
        if (wav->channels == 2U) {
            out[i * 2U] = (float)wav->data[index * 2U] / 32768.0f;
            out[i * 2U + 1U] = (float)wav->data[index * 2U + 1U] / 32768.0f;
        } else {
            out[i * 2U] = out[i * 2U + 1U] = (float)wav->data[index] / 32768.0f;
        }
        *phase += step;
    }
}

int main(void)
{
    static float local_block[LOCAL_BLOCK_FRAMES * UAC2_CHANNELS];
    struct local_wav_view local_wav;
    uint64_t local_phase = 0U;
    bool local_audio_ready;
    bool uac2_handover_logged = false;
    bool first_packet_logged = false;
    uint32_t last_packets = 0U;
    uint32_t last_frames = 0U;
    uint32_t last_invalid_packets = 0U;
    uint32_t last_odd_packets = 0U;
    int64_t next_status_ms = 0;
    static play_audio_pipeline pipeline;
    int ret;

    key_log("UAC2 receiver start");

#ifndef MINIAUDIO_UAC2_NULL_SINK
    ret = pcm5102a_audio_init();
    if (ret == 0) ret = play_audio_pipeline_init(&pipeline, UAC2_SAMPLE_RATE, UAC2_CHANNELS);
    if (ret == 0) ret = play_audio_pipeline_add_output(&pipeline, "pcm5102a", NULL,
                                                       pcm5102a_audio_output_stage);
    /* Must stay lower priority (higher number) than CONFIG_UDC_STM32_THREAD_PRIORITY
     * (8): this thread can block for a couple ms doing I2S/DMA writes, and if
     * it ran at higher priority than the USB stack thread it would preempt
     * and starve isochronous (micro)frame servicing.
     */
    if (ret == 0) ret = play_pipeline_async_init(&audio_out_async,
                                                 audio_out_stack, K_THREAD_STACK_SIZEOF(audio_out_stack),
                                                 audio_out_msgq_buffer, AUDIO_OUT_QUEUE_DEPTH,
                                                 audio_out_scratch,
                                                 LOCAL_BLOCK_FRAMES, UAC2_CHANNELS,
                                                 10, pipeline_sink, &pipeline);
#else
    ret = 0;
#endif
    if (ret != 0) {
        key_log("PCM5102A I2S init failed");
        return 0;
    }
#ifndef MINIAUDIO_UAC2_NULL_SINK
    key_log("PCM5102A ready");
#else
    key_log("UAC2 null sink");
#endif

    ret = parse_local_wav(&local_wav);
    local_audio_ready = (ret == 0);
    if (ret != 0) {
        key_log("Local audio parse failed");
    } else {
        key_log("Local audio playback");
    }

#ifndef MINIAUDIO_UAC2_NULL_SINK
    usb_uac2_set_audio_sink(play_pipeline_async_push, &audio_out_async);
#ifndef MINIAUDIO_UAC2_IMPLICIT
    /* Implicit feedback removes the OUT stream's explicit feedback endpoint
     * entirely (see zephyr,uac2-audio-streaming binding), so there is
     * nothing for usb_uac2_set_feedback_source() to drive in that mode.
     */
    usb_uac2_set_feedback_source(pcm5102a_fill_permille, NULL);
#endif
#else
    usb_uac2_set_audio_sink(NULL, NULL);
#endif
    ret = usb_uac2_device_init(UAC2_SAMPLE_RATE, UAC2_CHANNELS, 16U);
    if (ret != 0) {
        LOG_ERR("UAC2 device init failed: %d", ret);
        key_log("UAC2 device init failed");
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }

    key_log("UAC2 stream ready");
#ifdef MINIAUDIO_UAC2_IMPLICIT
    key_log("UAC2 feedback: implicit");
#else
    key_log("UAC2 feedback: explicit");
#endif
    next_status_ms = k_uptime_get();

    while (1) {
        if (usb_uac2_stream_active() && !uac2_handover_logged) {
            key_log("UAC2 host audio");
            uac2_handover_logged = true;
        }
        if (usb_uac2_first_packet_seen() && !first_packet_logged) {
            key_log("UAC2 first packet");
            first_packet_logged = true;
        }
        if (k_uptime_get() >= next_status_ms) {
            uint32_t packets;
            uint32_t bytes;
            uint32_t frames;
                uint32_t buf_requests;
                uint32_t buf_rejects;
                uint32_t zero_packets;
                uint32_t invalid_packets;
                uint32_t odd_byte_packets;
                uint32_t checksum;
                uint32_t pipeline_processed;
                uint32_t pipeline_failed;
                uint32_t audio_out_delivered = 0U;
                uint32_t slab_min = 0U;
                uint32_t slab_max = 0U;
                int32_t fb_fill_min = 0;
                int32_t fb_fill_max = 0;
                usb_uac2_get_stats(&packets, &bytes, &frames,
                           &buf_requests, &buf_rejects, &zero_packets,
                           &invalid_packets, &odd_byte_packets, &checksum);
                play_audio_pipeline_get_stats(&pipeline, &pipeline_processed, &pipeline_failed);
#ifndef MINIAUDIO_UAC2_NULL_SINK
                audio_out_delivered = play_pipeline_async_get_delivered(&audio_out_async);
                pcm5102a_audio_sample_slab_range(&slab_min, &slab_max);
#endif
                usb_uac2_sample_feedback_fill_range(&fb_fill_min, &fb_fill_max);
            {
                LOG_INF("UAC2 RX packets=%u bytes=%u frames=%u out_delivered=%u buffers=%u rejected=%u zero=%u invalid=%u odd=%u checksum=%u pipe_ok=%u pipe_fail=%u audio_drop=%u fb_adj=%d fb_calls=%u fb_fill_min=%d fb_fill_max=%d slab_min=%u slab_max=%u",
                    packets, bytes, frames, audio_out_delivered, buf_requests,
                    buf_rejects, zero_packets, invalid_packets, odd_byte_packets, checksum,
                    pipeline_processed, pipeline_failed, usb_uac2_get_audio_out_dropped(),
                    usb_uac2_get_feedback_adjust(), usb_uac2_get_feedback_call_count(),
                    fb_fill_min, fb_fill_max, slab_min, slab_max);
                last_packets = packets;
                last_frames = frames;
                last_invalid_packets = invalid_packets;
                last_odd_packets = odd_byte_packets;
            }
            next_status_ms += 500;
        }
    #ifndef MINIAUDIO_UAC2_NULL_SINK
        if (!usb_uac2_stream_active() && local_audio_ready) {
            fill_local_block(local_block, &local_wav, &local_phase);
            (void)play_audio_pipeline_process(&pipeline, local_block, LOCAL_BLOCK_FRAMES);
            k_sleep(K_MSEC(1));
            continue;
        }
    #else
        if (!usb_uac2_stream_active() && local_audio_ready && false) {
            continue;
        }
    #endif
        k_sleep(K_SECONDS(1));
    }
}
