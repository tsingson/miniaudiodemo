#include "cmsis_eq_stage.h"

#include <math.h>
#include <string.h>

#include "play_eq_normal.h"

int cmsis_eq_stage_init(cmsis_eq_stage *stage,
                        uint32_t sampleRate,
                        uint32_t channels,
                        float frequencyHz,
                        float gainDb,
                        float q)
{
    double b0;
    double b1;
    double b2;
    double a1;
    double a2;

    if (stage == NULL || channels == 0U || channels > 2U || sampleRate == 0U) {
        return -1;
    }
    memset(stage, 0, sizeof(*stage));
    stage->channels = channels;
    play_eq_normal_build_peaking(gainDb, q, frequencyHz, sampleRate,
                                 &b0, &b1, &b2, &a1, &a2);
    stage->coefficients[0] = (float)b0;
    stage->coefficients[1] = (float)b1;
    stage->coefficients[2] = (float)b2;
    stage->coefficients[3] = (float)-a1;
    stage->coefficients[4] = (float)-a2;
#if defined(USE_CMSIS_DSP) || defined(CONFIG_CMSIS_DSP)
    for (uint32_t channel = 0U; channel < channels; ++channel) {
        arm_biquad_cascade_df1_init_f32(&stage->filters[channel], 1U,
                                        stage->coefficients, stage->state[channel]);
    }
#else
    (void)frequencyHz;
    (void)gainDb;
    (void)q;
#endif
    return 0;
}

int cmsis_eq_stage_process(void *ctx, play_frame_block *block)
{
    cmsis_eq_stage *stage = ctx;

    if (stage == NULL || block == NULL || block->layout != PLAY_BUFFER_LAYOUT_PLANAR ||
        block->planar == NULL || block->frameCount == 0U || block->channels != stage->channels) {
        return -1;
    }
#if defined(USE_CMSIS_DSP) || defined(CONFIG_CMSIS_DSP)
    for (uint32_t channel = 0U; channel < stage->channels; ++channel) {
        arm_biquad_cascade_df1_f32(&stage->filters[channel],
                                   block->planar[channel], block->planar[channel],
                                   block->frameCount);
    }
#else
    (void)stage;
#endif
    return 0;
}
