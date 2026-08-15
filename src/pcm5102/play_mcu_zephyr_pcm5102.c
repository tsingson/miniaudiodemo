#include <stdint.h>

#include <zephyr/kernel.h>

#include "pcm5102a_audio.h"

uint32_t play_mcu_read_frames(float *interleaved_out, uint32_t max_frames)
{
    (void)interleaved_out;
    (void)max_frames;
    k_sleep(K_MSEC(1));
    return 0U;
}

void play_mcu_poll_eq(float *gains_out, float *qs_out, int max_bands)
{
    if (gains_out == NULL || qs_out == NULL || max_bands <= 0) {
        return;
    }
    for (int i = 0; i < max_bands; ++i) {
        gains_out[i] = 0.0f;
        qs_out[i] = 0.707f;
    }
}

/* PCM5102A compatibility symbols are implemented by pcm5102a_audio.c. */
