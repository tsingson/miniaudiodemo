#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_4_wav_data.h"
#include "pcm5102a_audio.h"
#include "play_audio_pipeline.h"
#include "st7735s_log_display.h"

LOG_MODULE_REGISTER(main_pcm5102a_test, LOG_LEVEL_INF);

#define TEST_RATE 48000U
#define TEST_FRAMES 128U
#define TEST_CHANNELS 2U

struct wav_view {
    const int16_t *pcm;
    uint32_t frames;
    uint16_t channels;
    uint32_t sample_rate;
};

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8U);
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

static int parse_wav(struct wav_view *wav)
{
    const unsigned char *data = g_audio_4_wav;
    uint32_t pos = 12U;
    uint16_t format = 0U;
    uint16_t channels = 0U;
    uint16_t bits = 0U;
    uint32_t rate = 0U;
    const int16_t *pcm = NULL;
    uint32_t bytes = 0U;

    if (g_audio_4_wav_len < 44U || memcmp(data, "RIFF", 4) != 0 ||
        memcmp(data + 8U, "WAVE", 4) != 0) return -EINVAL;
    while (pos + 8U <= g_audio_4_wav_len) {
        uint32_t size = read_le32(data + pos + 4U);
        uint32_t payload = pos + 8U;
        if (payload + size > g_audio_4_wav_len) return -EINVAL;
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
    if (format != 1U || channels != TEST_CHANNELS || bits != 16U ||
        rate != TEST_RATE || pcm == NULL) return -EINVAL;
    wav->pcm = pcm;
    wav->channels = channels;
    wav->sample_rate = rate;
    wav->frames = bytes / (TEST_CHANNELS * sizeof(int16_t));
    return wav->frames > 0U ? 0 : -EINVAL;
}

static void fill_block(float *out, const struct wav_view *wav, uint32_t *position)
{
    for (uint32_t frame = 0U; frame < TEST_FRAMES; ++frame) {
        uint32_t source = *position;
        out[frame * TEST_CHANNELS] = (float)wav->pcm[source * TEST_CHANNELS] / 32768.0f;
        out[frame * TEST_CHANNELS + 1U] =
            (float)wav->pcm[source * TEST_CHANNELS + 1U] / 32768.0f;
        *position = (source + 1U) % wav->frames;
    }
}

int main(void)
{
    static float block[TEST_FRAMES * TEST_CHANNELS];
    static play_audio_pipeline pipeline;
    struct wav_view wav;
    uint32_t position = 0U;
    int64_t next_status_ms;
    int ret;

    (void)st7735s_log_display_init();
    if (parse_wav(&wav) != 0) {
        st7735s_log_display_line("4.wav parse fail");
        return 0;
    }
    ret = pcm5102a_audio_init();
    if (ret == 0) ret = play_audio_pipeline_init(&pipeline, TEST_RATE, TEST_CHANNELS);
    if (ret == 0) ret = play_audio_pipeline_add_dsp(&pipeline);
    if (ret == 0) ret = play_audio_pipeline_add_output(&pipeline, "pcm5102a", NULL,
                                                       pcm5102a_audio_output_stage);
    if (ret != 0) {
        LOG_ERR("audio pipeline init failed: %d", ret);
        st7735s_log_display_line("pipeline init fail");
        return 0;
    }

    LOG_INF("WAV source: 440 Hz PCM16 stereo 48 kHz");
    st7735s_log_display_line("WAV 440Hz 48k");
    st7735s_log_display_line("I2S running");
    next_status_ms = k_uptime_get() + 1000;
    while (1) {
        fill_block(block, &wav, &position);
        (void)play_audio_pipeline_process(&pipeline, block, TEST_FRAMES);
        if (k_uptime_get() >= next_status_ms) {
            uint32_t blocks;
            uint32_t errors;
            char status[32];
            pcm5102a_audio_get_stats(&blocks, &errors);
            (void)snprintf(status, sizeof(status), "I2S %u/%u", blocks, errors);
            st7735s_log_display_line(status);
            next_status_ms += 1000;
        }
    }
}
