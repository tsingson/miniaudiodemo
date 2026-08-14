#ifndef PCM5102A_AUDIO_H_
#define PCM5102A_AUDIO_H_

#include <stdint.h>

#include "dspeq/play_pipeline.h"

int pcm5102a_audio_init(void);
void pcm5102a_audio_write_float(const float *interleaved, uint32_t frames);
void pcm5102a_audio_write_pcm16(const int16_t *interleaved, uint32_t frames);
int pcm5102a_audio_output_stage(void *ctx, play_frame_block *block);
void pcm5102a_audio_get_stats(uint32_t *write_blocks, uint32_t *write_errors);
uint32_t pcm5102a_audio_get_slab_used(void);
uint32_t pcm5102a_audio_get_slab_capacity(void);

/* Diagnostic: min/max TX slab occupancy observed since the last call, then
 * resets the tracked range. Lets callers directly see whether the buffer is
 * genuinely trending toward empty (clock drift) or just oscillating.
 */
void pcm5102a_audio_sample_slab_range(uint32_t *min_used, uint32_t *max_used);

#endif
