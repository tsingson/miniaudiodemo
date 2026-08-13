#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tusb.h"
#include "play_audio_pipeline.h"

LOG_MODULE_REGISTER(main_tinyusb_uac2, LOG_LEVEL_INF);

#define TINYUSB_SAMPLE_RATE 48000U
#define TINYUSB_CHANNELS 2U
#define TINYUSB_BLOCK_FRAMES 48U

extern int play_audio_pipeline_process(play_audio_pipeline *pipeline,
                                       const float *interleaved,
                                       uint32_t frames);

int main(void)
{
    static int16_t pcm[TINYUSB_BLOCK_FRAMES * TINYUSB_CHANNELS];
    static float block[TINYUSB_BLOCK_FRAMES * TINYUSB_CHANNELS];
    static play_audio_pipeline pipeline;
    uint32_t frame_count;

    if (!tusb_init()) {
        LOG_ERR("TinyUSB init failed");
        return 0;
    }
    if (play_audio_pipeline_init(&pipeline, TINYUSB_SAMPLE_RATE, TINYUSB_CHANNELS) != 0) {
        LOG_ERR("pipeline init failed");
        return 0;
    }

    LOG_INF("TinyUSB UAC2 ready: 48k stereo PCM16");
    while (1) {
        tud_task();
        frame_count = tud_audio_read(pcm, sizeof(pcm)) /
                      (sizeof(int16_t) * TINYUSB_CHANNELS);
        if (frame_count > 0U) {
            for (uint32_t sample = 0U; sample < frame_count * TINYUSB_CHANNELS; ++sample) {
                block[sample] = (float)pcm[sample] / 32768.0f;
            }
            (void)play_audio_pipeline_process(&pipeline, block, frame_count);
        }
        k_sleep(K_MSEC(1));
    }
}
