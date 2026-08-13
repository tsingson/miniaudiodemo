#ifndef PCM5102A_AUDIO_H_
#define PCM5102A_AUDIO_H_

#include <stdint.h>

#include "play_pipeline.h"

int pcm5102a_audio_init(void);
void pcm5102a_audio_write_float(const float *interleaved, uint32_t frames);
void pcm5102a_audio_write_pcm16(const int16_t *interleaved, uint32_t frames);
int pcm5102a_audio_output_stage(void *ctx, play_frame_block *block);
void pcm5102a_audio_get_stats(uint32_t *write_blocks, uint32_t *write_errors);

#endif
