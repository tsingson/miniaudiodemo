#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_3_wav_data.h"
#include "st7735s_log_display.h"
#include "usb_uac2_device.h"
#include "play_audio_pipeline.h"
#include "pcm5102a_audio.h"

LOG_MODULE_REGISTER(main_pcm5102_st7735s_uac2, LOG_LEVEL_INF);

#define UAC2_SAMPLE_RATE 48000U
#define UAC2_CHANNELS 2U
#define LOCAL_BLOCK_FRAMES 128U

static bool g_display_ready;

static int pipeline_sink(void *ctx, const float *frames, uint32_t count)
{
    return play_audio_pipeline_process((play_audio_pipeline *)ctx, frames, count);
}

struct local_wav_view {
    const int16_t *data;
    uint32_t frames;
    uint16_t channels;
    uint32_t sample_rate;
};

static void key_log(const char *message)
{
    LOG_INF("%s", message);
    if (g_display_ready) {
        st7735s_log_display_line(message);
    }
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
    uint32_t status_ticks = 0U;
    uint32_t last_frames = 0U;
    int64_t last_rate_ms = 0;
    play_audio_pipeline pipeline;
    int ret;

    if (st7735s_log_display_init() == 0) {
        g_display_ready = true;
    }

    key_log("UAC2 receiver start");

    ret = pcm5102a_audio_init();
    if (ret == 0) ret = play_audio_pipeline_init(&pipeline, UAC2_SAMPLE_RATE, UAC2_CHANNELS);
    if (ret == 0) ret = play_audio_pipeline_add_dsp(&pipeline);
    if (ret == 0) ret = play_audio_pipeline_add_output(&pipeline, "pcm5102a", NULL,
                                                       pcm5102a_audio_output_stage);
    if (ret != 0) {
        key_log("PCM5102A I2S init failed");
        return 0;
    }
    key_log("PCM5102A ready");

    ret = parse_local_wav(&local_wav);
    local_audio_ready = (ret == 0);
    if (ret != 0) {
        key_log("Local audio parse failed");
    } else {
        key_log("Local audio playback");
    }

    usb_uac2_set_audio_sink(pipeline_sink, &pipeline);
    ret = usb_uac2_device_init(UAC2_SAMPLE_RATE, UAC2_CHANNELS, 16U);
    if (ret != 0) {
        LOG_ERR("UAC2 device init failed: %d", ret);
        key_log("UAC2 device init failed");
        while (1) {
            k_sleep(K_SECONDS(1));
        }
    }

    key_log("UAC2 stream ready");

    while (1) {
        if (usb_uac2_stream_active() && !uac2_handover_logged) {
            key_log("UAC2 host audio");
            uac2_handover_logged = true;
        }
        if (usb_uac2_first_packet_seen() && !first_packet_logged) {
            key_log("UAC2 first packet");
            first_packet_logged = true;
        }
        status_ticks++;
        if (usb_uac2_stream_active() || status_ticks >= 5U) {
            uint32_t packets;
            uint32_t bytes;
            uint32_t frames;
            uint32_t i2s_blocks;
                uint32_t buf_requests;
                uint32_t buf_rejects;
                uint32_t zero_packets;
                usb_uac2_get_stats(&packets, &bytes, &frames, &i2s_blocks,
                           &buf_requests, &buf_rejects, &zero_packets);
            if (packets != last_packets || status_ticks >= 5U) {
                char line[32];
                int64_t now_ms = k_uptime_get();
                uint32_t rate = 0U;
                if (last_rate_ms > 0 && now_ms > last_rate_ms) {
                    rate = (uint32_t)(((uint64_t)(frames - last_frames) * 1000U) /
                                      (uint64_t)(now_ms - last_rate_ms));
                }
                LOG_INF("UAC2 RX packets=%u bytes=%u frames=%u I2S blocks=%u buffers=%u rejected=%u zero=%u",
                    packets, bytes, frames, i2s_blocks, buf_requests,
                    buf_rejects, zero_packets);
                (void)snprintf(line, sizeof(line), "RX p%u b%u", packets, bytes);
                st7735s_log_display_line(line);
                (void)snprintf(line, sizeof(line), "I2S n%u e%u", i2s_blocks, buf_rejects);
                st7735s_log_display_line(line);
                (void)snprintf(line, sizeof(line), "RX z%u f%u", zero_packets, frames);
                st7735s_log_display_line(line);
                (void)snprintf(line, sizeof(line), "RATE %uHz", rate);
                st7735s_log_display_line(line);
                last_packets = packets;
                last_frames = frames;
                last_rate_ms = now_ms;
                status_ticks = 0U;
            }
        }
        if (!usb_uac2_stream_active() && local_audio_ready) {
            fill_local_block(local_block, &local_wav, &local_phase);
            (void)play_audio_pipeline_process(&pipeline, local_block, LOCAL_BLOCK_FRAMES);
            continue;
        }
        k_sleep(K_SECONDS(1));
    }
}
