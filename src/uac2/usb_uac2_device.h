#ifndef USB_UAC2_DEVICE_H_
#define USB_UAC2_DEVICE_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef int (*usb_uac2_audio_sink_fn)(void *ctx, const float *interleaved, uint32_t frames);

void usb_uac2_set_audio_sink(usb_uac2_audio_sink_fn sink, void *ctx);

/* Optional: lets the feedback endpoint track a downstream buffer-fill signal
 * (0-1000 per-mille) without UAC2 depending on any concrete output module.
 * Whoever wires input+output together (the composition root) supplies this,
 * e.g. play_pipeline_async_fill_permille() bound to the output queue.
 */
typedef uint32_t (*usb_uac2_fill_query_fn)(void *ctx);
void usb_uac2_set_feedback_source(usb_uac2_fill_query_fn query, void *ctx);

int usb_uac2_device_init(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
bool usb_uac2_stream_active(void);
void usb_uac2_get_stats(uint32_t *packets, uint32_t *bytes,
						uint32_t *frames,
						uint32_t *buf_requests, uint32_t *buf_rejects,
						uint32_t *zero_packets, uint32_t *invalid_packets,
						uint32_t *odd_byte_packets, uint32_t *checksum);
bool usb_uac2_first_packet_seen(void);
uint32_t usb_uac2_get_audio_out_dropped(void);
int32_t usb_uac2_get_feedback_adjust(void);
uint32_t usb_uac2_get_feedback_call_count(void);
void usb_uac2_sample_feedback_fill_range(int32_t *min_fill, int32_t *max_fill);

#endif
