#ifndef CMSIS_EQ_STAGE_H_
#define CMSIS_EQ_STAGE_H_

#include <stdint.h>

#include "play_pipeline.h"

#if defined(USE_CMSIS_DSP) || defined(CONFIG_CMSIS_DSP)
#include <arm_math.h>
#endif

typedef struct
{
    uint32_t channels;
    float coefficients[5];
#if defined(USE_CMSIS_DSP) || defined(CONFIG_CMSIS_DSP)
    arm_biquad_casd_df1_inst_f32 filters[2];
    float state[2][4];
#endif
} cmsis_eq_stage;

int cmsis_eq_stage_init(cmsis_eq_stage *stage,
                        uint32_t sampleRate,
                        uint32_t channels,
                        float frequencyHz,
                        float gainDb,
                        float q);
int cmsis_eq_stage_process(void *ctx, play_frame_block *block);

#endif
