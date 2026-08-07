#include "play_dsp_common.h"

#include <stdint.h>
#include <string.h>

#define MCU_CHANNELS 2
#define MCU_BLOCK_FRAMES 128
#define MCU_SAMPLE_RATE 48000

__attribute__((weak)) uint32_t play_mcu_read_frames(float* interleavedOut, uint32_t maxFrames)
{
    (void)interleavedOut;
    (void)maxFrames;
    return 0;
}

__attribute__((weak)) void play_mcu_write_frames(const float* interleavedIn, uint32_t frameCount)
{
    (void)interleavedIn;
    (void)frameCount;
}

__attribute__((weak)) void play_mcu_poll_eq(float* gainsOut, float* qsOut, int maxBands)
{
    (void)gainsOut;
    (void)qsOut;
    (void)maxBands;
}

int main(void)
{
    play_dsp_state dsp;
    float audioBlock[MCU_BLOCK_FRAMES * MCU_CHANNELS];
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];

    if (play_dsp_init(&dsp, MCU_SAMPLE_RATE, MCU_CHANNELS) != 0)
    {
        return -1;
    }

    memset(gains, 0, sizeof(gains));
    memset(qs, 0, sizeof(qs));

    for (;;)
    {
        uint32_t frames = play_mcu_read_frames(audioBlock, MCU_BLOCK_FRAMES);

        play_mcu_poll_eq(gains, qs, EQ_BANDS);
        play_dsp_set_eq_params(&dsp, gains, EQ_BANDS, qs, EQ_BANDS);

        if (frames > 0)
        {
            play_dsp_process(&dsp, audioBlock, frames, MCU_CHANNELS);
            play_mcu_write_frames(audioBlock, frames);
        }
    }
}
