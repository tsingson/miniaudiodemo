#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "audio_3_wav_data.h"
#include "st7735s_log_display.h"

LOG_MODULE_REGISTER(main_pcm5102_st7735s, LOG_LEVEL_INF);

#define OUT_SAMPLE_RATE 48000U
#define OUT_CHANNELS 2U
#define OUT_BLOCK_FRAMES 128U
#define PLAYBACK_GAIN 0.35f

void play_mcu_write_frames(const float *interleavedIn, uint32_t frameCount);
int play_mcu_pcm5102a_init(void);

static bool g_lcd_ready;

struct wav_pcm_view {
    const int16_t *data;
    uint32_t frames;
    uint16_t channels;
    uint32_t sample_rate;
};

static void log_key(const char *msg)
{
    LOG_INF("%s", msg);
    if (g_lcd_ready) {
        st7735s_log_display_line(msg);
    }
}

static uint16_t rd16_le(const unsigned char *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32_le(const unsigned char *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int parse_wav_pcm16_stereo_or_mono(struct wav_pcm_view *out)
{
    const unsigned char *p = g_audio_3_wav;
    uint32_t len = g_audio_3_wav_len;
    uint32_t pos = 12U;
    uint16_t audio_format = 0U;
    uint16_t channels = 0U;
    uint16_t bits_per_sample = 0U;
    uint32_t sample_rate = 0U;
    const int16_t *data_ptr = NULL;
    uint32_t data_size = 0U;

    if (len < 44U) {
        return -1;
    }

    if ((memcmp(p, "RIFF", 4) != 0) || (memcmp(p + 8, "WAVE", 4) != 0)) {
        return -2;
    }

    while ((pos + 8U) <= len) {
        const unsigned char *chunk = p + pos;
        uint32_t chunk_size = rd32_le(chunk + 4);
        uint32_t chunk_data = pos + 8U;

        if (chunk_data + chunk_size > len) {
            return -3;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16U) {
                return -4;
            }
            audio_format = rd16_le(p + chunk_data + 0U);
            channels = rd16_le(p + chunk_data + 2U);
            sample_rate = rd32_le(p + chunk_data + 4U);
            bits_per_sample = rd16_le(p + chunk_data + 14U);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data_ptr = (const int16_t *)(p + chunk_data);
            data_size = chunk_size;
        }

        pos = chunk_data + chunk_size + (chunk_size & 1U);
    }

    if (audio_format != 1U || (channels != 1U && channels != 2U) || bits_per_sample != 16U || data_ptr == NULL) {
        return -5;
    }

    out->data = data_ptr;
    out->channels = channels;
    out->sample_rate = sample_rate;
    out->frames = data_size / ((uint32_t)channels * sizeof(int16_t));
    return (out->frames > 0U) ? 0 : -6;
}

static void fill_output_from_wav(float *out,
                                 uint32_t frames,
                                 const struct wav_pcm_view *wav,
                                 uint64_t *phase_q32,
                                 uint64_t step_q32)
{
    uint64_t phase_wrap = ((uint64_t)wav->frames << 32);

    for (uint32_t i = 0U; i < frames; ++i) {
        uint32_t si;
        float l;
        float r;

        while (*phase_q32 >= phase_wrap) {
            *phase_q32 -= phase_wrap;
        }

        si = (uint32_t)(*phase_q32 >> 32);

        if (wav->channels == 2U) {
            l = (float)wav->data[si * 2U + 0U] / 32768.0f;
            r = (float)wav->data[si * 2U + 1U] / 32768.0f;
        } else {
            l = (float)wav->data[si] / 32768.0f;
            r = l;
        }

        out[i * OUT_CHANNELS + 0U] = l * PLAYBACK_GAIN;
        out[i * OUT_CHANNELS + 1U] = r * PLAYBACK_GAIN;

        *phase_q32 += step_q32;
    }
}

int main(void)
{
    static float block[OUT_BLOCK_FRAMES * OUT_CHANNELS];
    struct wav_pcm_view wav;
    uint64_t phase_q32 = 0U;
    uint64_t step_q32;
    char line[48];

    if (st7735s_log_display_init() == 0) {
        g_lcd_ready = true;
    }

    if (parse_wav_pcm16_stereo_or_mono(&wav) != 0) {
        log_key("3.wav parse failed");
        while (1) {
            k_sleep(K_MSEC(1000));
        }
    }

    step_q32 = ((uint64_t)wav.sample_rate << 32) / OUT_SAMPLE_RATE;

    log_key("PCM5102A start");
    (void)snprintf(line, sizeof(line), "wav: %uHz ch=%u", (unsigned int)wav.sample_rate, (unsigned int)wav.channels);
    log_key(line);
    if (play_mcu_pcm5102a_init() != 0) {
        log_key("PCM5102A I2S init failed");
        while (1) {
            k_sleep(K_MSEC(1000));
        }
    }

    log_key("PCM5102A ready");

    while (1) {
        fill_output_from_wav(block, OUT_BLOCK_FRAMES, &wav, &phase_q32, step_q32);
        play_mcu_write_frames(block, OUT_BLOCK_FRAMES);
    }
}
