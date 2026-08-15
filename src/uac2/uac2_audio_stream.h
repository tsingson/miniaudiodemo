#ifndef UAC2_AUDIO_STREAM_H_
#define UAC2_AUDIO_STREAM_H_

#include <stdbool.h>
#include <stdint.h>

#define UAC2_AUDIO_SAMPLE_RATE 48000U
#define UAC2_AUDIO_CHANNELS 2U
#define UAC2_AUDIO_BITS_PER_SAMPLE 16U
#define UAC2_AUDIO_BLOCK_FRAMES 128U

typedef int (*uac2_audio_process_fn)(void *ctx,
                                     const float *interleaved,
                                     uint32_t frames);
typedef uint32_t (*uac2_audio_fill_query_fn)(void *ctx);

struct uac2_audio_stream_config {
    uac2_audio_process_fn process;
    void *process_ctx;
    uac2_audio_fill_query_fn feedback_fill;
    void *feedback_ctx;
};

struct uac2_audio_stream_stats {
    uint32_t packets;
    uint32_t bytes;
    uint32_t frames;
    uint32_t delivered;
    uint32_t buffer_requests;
    uint32_t buffer_rejects;
    uint32_t zero_packets;
    uint32_t invalid_packets;
    uint32_t odd_byte_packets;
    uint32_t checksum;
    uint32_t dropped;
    int32_t feedback_adjust;
    uint32_t feedback_calls;
};

int uac2_audio_stream_init(const struct uac2_audio_stream_config *config);
bool uac2_audio_stream_active(void);
bool uac2_audio_stream_first_packet_seen(void);
void uac2_audio_stream_get_stats(struct uac2_audio_stream_stats *stats);
void uac2_audio_stream_sample_feedback_fill_range(int32_t *min_fill,
                                                  int32_t *max_fill);

#endif
