#include "play_dsp_common.h"
#include "play_audio_pipeline.h"
#include "pcm5102a_audio.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#define MCU_CHANNELS 2
#define MCU_BLOCK_FRAMES 128
#define MCU_SAMPLE_RATE 48000

/*
 * Keep large DSP working buffers in static storage for MCU targets.
 * This avoids early stack overflow in main() on small embedded stacks.
 */
static float g_audioBlock[MCU_BLOCK_FRAMES * MCU_CHANNELS];
static float g_gains[EQ_BANDS];
static float g_qs[EQ_BANDS];
static float g_prevGains[EQ_BANDS];
static float g_prevQs[EQ_BANDS];
static play_audio_pipeline g_pipeline;

__attribute__ ((weak)) uint32_t play_mcu_read_frames(float* interleavedOut, uint32_t maxFrames)
{
    (void)interleavedOut;
    (void)maxFrames;
    return 0;
}

__attribute__ ((weak))

void play_mcu_write_frames(const float* interleavedIn, uint32_t frameCount)
{
    (void)interleavedIn;
    (void)frameCount;
}

__attribute__ ((weak))

void play_mcu_poll_eq(float* gainsOut, float* qsOut, int maxBands)
{
    (void)gainsOut;
    (void)qsOut;
    (void)maxBands;
}

int main(void)
{
    int eqInitialized = 0;

    if (play_audio_pipeline_init(&g_pipeline, MCU_SAMPLE_RATE, MCU_CHANNELS) != 0 ||
        play_audio_pipeline_add_dsp(&g_pipeline) != 0 ||
        play_audio_pipeline_add_output(&g_pipeline, "pcm5102a", NULL,
                                       pcm5102a_audio_output_stage) != 0)
    {
        return -1;
    }

    memset(g_gains, 0, sizeof(g_gains));
    memset(g_qs, 0, sizeof(g_qs));
    memset(g_prevGains, 0, sizeof(g_prevGains));
    memset(g_prevQs, 0, sizeof(g_prevQs));

    for (;;)
    {
        uint32_t frames = play_mcu_read_frames(g_audioBlock, MCU_BLOCK_FRAMES);

        if (frames > MCU_BLOCK_FRAMES)
        {
            frames = MCU_BLOCK_FRAMES;
        }

        play_mcu_poll_eq(g_gains, g_qs, EQ_BANDS);

        {
            int changed = !eqInitialized;

            for (int i = 0; i < EQ_BANDS && !changed; ++i)
            {
                if (fabsf(g_gains[i] - g_prevGains[i]) > 1e-6f || fabsf(g_qs[i] - g_prevQs[i]) > 1e-6f)
                {
                    changed = 1;
                }
            }

            if (changed)
            {
                play_dsp_set_eq_params(&g_pipeline.dsp, g_gains, EQ_BANDS, g_qs, EQ_BANDS);
                memcpy(g_prevGains, g_gains, sizeof(g_prevGains));
                memcpy(g_prevQs, g_qs, sizeof(g_prevQs));
                eqInitialized = 1;
            }
        }

        if (frames > 0)
        {
            (void)play_audio_pipeline_process(&g_pipeline, g_audioBlock, frames);
        }
    }
}
