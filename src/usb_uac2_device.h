#ifndef USB_UAC2_DEVICE_H_
#define USB_UAC2_DEVICE_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef int (*usb_uac2_audio_sink_fn)(void *ctx, const float *interleaved, uint32_t frames);

void usb_uac2_set_audio_sink(usb_uac2_audio_sink_fn sink, void *ctx);

int usb_uac2_device_init(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample);
bool usb_uac2_stream_active(void);
void usb_uac2_get_stats(uint32_t *packets, uint32_t *bytes,
						uint32_t *frames, uint32_t *i2s_blocks,
						uint32_t *buf_requests, uint32_t *buf_rejects,
						uint32_t *zero_packets);
bool usb_uac2_first_packet_seen(void);

#endif
