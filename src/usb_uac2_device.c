#include "usb_uac2_device.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/class/usbd_uac2.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/usb_buf.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_uac2_device, LOG_LEVEL_INF);

#ifndef MINIAUDIO_UAC2_PID
#define MINIAUDIO_UAC2_PID 0x0102
#endif

#define UAC2_INPUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(uac2_input))
#define UAC2_MAX_PACKET_SIZE 200U
#define UAC2_BLOCK_FRAMES 48U
#define UAC2_CHANNELS 2U
#define UAC2_I2S_BLOCK_FRAMES 128U
#define UAC2_FS_FEEDBACK (48U << 14U)

K_MEM_SLAB_DEFINE_STATIC(uac2_rx_slab,
             ROUND_UP(UAC2_MAX_PACKET_SIZE, UDC_BUF_GRANULARITY),
             32, UDC_BUF_ALIGN);

static float pending_samples[UAC2_I2S_BLOCK_FRAMES * UAC2_CHANNELS];
static size_t pending_frames;
static atomic_t g_audio_out_dropped;

static atomic_t g_uac2_stream_active;
static atomic_t g_packet_count;
static atomic_t g_byte_count;
static atomic_t g_frame_count;
static atomic_t g_buf_requests;
static atomic_t g_buf_rejects;
static atomic_t g_buf_releases;
static atomic_t g_zero_packets;
static atomic_t g_invalid_packets;
static atomic_t g_odd_byte_packets;
static atomic_t g_checksum;
static atomic_t g_first_packet_logged;
static atomic_t g_sof_seen;
static atomic_t g_terminal_enabled;
static usb_uac2_audio_sink_fn g_audio_sink;
static void *g_audio_sink_ctx;
static usb_uac2_fill_query_fn g_feedback_query;
static void *g_feedback_query_ctx;

USBD_DEVICE_DEFINE(uac2_device,
           DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
           0x2FE3, MINIAUDIO_UAC2_PID);
USBD_DESC_LANG_DEFINE(uac2_lang);
USBD_DESC_MANUFACTURER_DEFINE(uac2_mfr, "miniaudiodemo");
USBD_DESC_PRODUCT_DEFINE(uac2_product, "STM32F401 PCM5102A UAC2");
USBD_DESC_CONFIG_DEFINE(uac2_config_desc, "UAC2 Audio");
USBD_CONFIGURATION_DEFINE(uac2_config, 0, 100, &uac2_config_desc);

static const char *const uac2_blocklist[] = {
    "cdc_acm_0",
    NULL,
};

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    if (atomic_cas(&g_sof_seen, 0, 1)) {
        LOG_INF("UAC2 USB SOF detected");
    }
}

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal,
                    bool enabled, bool microframes, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(microframes);
    ARG_UNUSED(user_data);
    if (terminal == UAC2_INPUT_TERMINAL_ID) {
        printk("UAC2 terminal %u %s\n", terminal, enabled ? "enabled" : "disabled");
        atomic_set(&g_terminal_enabled, enabled ? 1 : 0);
        if (!enabled) {
            atomic_set(&g_uac2_stream_active, 0);
        }
    }
}

static void *uac2_get_recv_buf(const struct device *dev, uint8_t terminal,
                       uint16_t size, void *user_data)
{
    void *buf = NULL;

    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    atomic_inc(&g_buf_requests);
    if (terminal != UAC2_INPUT_TERMINAL_ID || size > UAC2_MAX_PACKET_SIZE) {
        atomic_inc(&g_buf_rejects);
        LOG_WRN("UAC2 RX buffer rejected terminal=%u size=%u", terminal, size);
        return NULL;
    }

    if (k_mem_slab_alloc(&uac2_rx_slab, &buf, K_NO_WAIT) != 0) {
        atomic_inc(&g_buf_rejects);
        return NULL;
    }
    return buf;
}

static void uac2_data_recv_cb(const struct device *dev, uint8_t terminal,
                      void *buf, uint16_t size, void *user_data)
{
    const int16_t *pcm = buf;
    size_t frames;

    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    if (terminal != UAC2_INPUT_TERMINAL_ID || buf == NULL) {
        atomic_inc(&g_invalid_packets);
        return;
    }

    if (size > 0U) {
        atomic_set(&g_uac2_stream_active, 1);
        atomic_inc(&g_packet_count);
        atomic_add(&g_byte_count, size);
        if (atomic_cas(&g_first_packet_logged, 0, 1)) {
            LOG_INF("UAC2 first PCM packet size=%u sample0=%d sample1=%d",
                    size, pcm[0], size >= 4U ? pcm[1] : 0);
        }
    } else {
        atomic_inc(&g_zero_packets);
    }

    if ((size % (sizeof(int16_t) * UAC2_CHANNELS)) != 0U) {
        atomic_inc(&g_odd_byte_packets);
        atomic_inc(&g_invalid_packets);
        k_mem_slab_free(&uac2_rx_slab, buf);
        return;
    }
    /* Local accumulate then a single atomic_add: this runs on the real-time
     * USB receive path, and per-byte atomic ops (LDREX/STREX retry) here were
     * adding enough overhead per packet to risk missing the next (micro)frame.
     */
    {
        uint32_t sum = 0U;
        for (uint16_t byte = 0U; byte < size; ++byte) {
            sum += ((const uint8_t *)buf)[byte];
        }
        atomic_add(&g_checksum, sum);
    }
    frames = size / (sizeof(int16_t) * UAC2_CHANNELS);
    for (size_t i = 0U; i < frames; ++i) {
        pending_samples[pending_frames * UAC2_CHANNELS] =
            (float)pcm[i * UAC2_CHANNELS] / 32768.0f;
        pending_samples[pending_frames * UAC2_CHANNELS + 1U] =
            (float)pcm[i * UAC2_CHANNELS + 1U] / 32768.0f;
        pending_frames++;

        if (pending_frames == UAC2_I2S_BLOCK_FRAMES) {
            /* Handed straight to whatever sink was registered (the pipeline
             * layer owns any buffering/decoupling it needs, e.g. via
             * play_pipeline_async_push()); this call must not block.
             */
            if (g_audio_sink == NULL ||
                g_audio_sink(g_audio_sink_ctx, pending_samples, UAC2_I2S_BLOCK_FRAMES) != 0) {
                atomic_inc(&g_audio_out_dropped);
            }
            pending_frames = 0U;
        }
    }
    atomic_add(&g_frame_count, frames);
    /* Class layer hands OUT buffer ownership to data_recv_cb (no buf_release_cb
     * for OUT); we must return it to our own pool ourselves.
     */
    k_mem_slab_free(&uac2_rx_slab, buf);
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal,
                                void *buf, void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(user_data);
    if (terminal == UAC2_INPUT_TERMINAL_ID && buf != NULL) {
        atomic_inc(&g_buf_releases);
        k_mem_slab_free(&uac2_rx_slab, buf);
    }
}

uint32_t usb_uac2_get_release_count(void)
{
    return (uint32_t)atomic_get(&g_buf_releases);
}

uint32_t usb_uac2_get_audio_out_dropped(void)
{
    return (uint32_t)atomic_get(&g_audio_out_dropped);
}

void usb_uac2_set_audio_sink(usb_uac2_audio_sink_fn sink, void *ctx)
{
    g_audio_sink = sink;
    g_audio_sink_ctx = ctx;
}

void usb_uac2_set_feedback_source(usb_uac2_fill_query_fn query, void *ctx)
{
    g_feedback_query = query;
    g_feedback_query_ctx = ctx;
}

/* Adaptive feedback: STM32 has no hardware SOF/I2S-frame timestamp comparator
 * (unlike e.g. the nRF reference sample), so approximate the true USB-vs-
 * output clock ratio by nudging the reported rate towards whichever
 * direction keeps the registered downstream buffer (see
 * usb_uac2_set_feedback_source()) near half full. Proportional integrator,
 * clamped to a wider range than typical USB PLL tolerances since this MCU's
 * I2S clock derives from the untrimmed HSI RC oscillator (no crystal), which
 * can be off by a few percent rather than the usual sub-1% crystal drift.
 */
#define UAC2_FB_MAX_ADJUST ((int32_t)(UAC2_FS_FEEDBACK / 20)) /* +-5% */
#define UAC2_FB_GAIN 16
#define UAC2_FB_TARGET_PERMILLE 500
static int32_t g_fb_adjust;

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
                                 void *user_data)
{
    int32_t fill;
    int32_t error;

    ARG_UNUSED(dev);
    ARG_UNUSED(terminal);
    ARG_UNUSED(user_data);

    if (g_feedback_query == NULL) {
        return UAC2_FS_FEEDBACK;
    }
    fill = (int32_t)g_feedback_query(g_feedback_query_ctx);
    error = UAC2_FB_TARGET_PERMILLE - fill;

    g_fb_adjust += (error * UAC2_FB_GAIN) / UAC2_FB_TARGET_PERMILLE;
    if (g_fb_adjust > UAC2_FB_MAX_ADJUST) {
        g_fb_adjust = UAC2_FB_MAX_ADJUST;
    } else if (g_fb_adjust < -UAC2_FB_MAX_ADJUST) {
        g_fb_adjust = -UAC2_FB_MAX_ADJUST;
    }

    return (uint32_t)((int32_t)UAC2_FS_FEEDBACK + g_fb_adjust);
}

int32_t usb_uac2_get_feedback_adjust(void)
{
    return g_fb_adjust;
}

static const struct uac2_ops uac2_ops = {
    .sof_cb = uac2_sof_cb,
    .terminal_update_cb = uac2_terminal_update_cb,
    .get_recv_buf = uac2_get_recv_buf,
    .data_recv_cb = uac2_data_recv_cb,
    .buf_release_cb = uac2_buf_release_cb,
    .feedback_cb = uac2_feedback_cb,
};

bool usb_uac2_stream_active(void)
{
    return atomic_get(&g_uac2_stream_active) != 0;
}

bool usb_uac2_terminal_enabled(void)
{
    return atomic_get(&g_terminal_enabled) != 0;
}

bool usb_uac2_first_packet_seen(void)
{
    return atomic_get(&g_first_packet_logged) != 0;
}

void usb_uac2_get_stats(uint32_t *packets, uint32_t *bytes,
                        uint32_t *frames,
                        uint32_t *buf_requests, uint32_t *buf_rejects,
                        uint32_t *zero_packets, uint32_t *invalid_packets,
                        uint32_t *odd_byte_packets, uint32_t *checksum)
{
    if (packets != NULL) {
        *packets = (uint32_t)atomic_get(&g_packet_count);
    }
    if (bytes != NULL) {
        *bytes = (uint32_t)atomic_get(&g_byte_count);
    }
    if (frames != NULL) {
        *frames = (uint32_t)atomic_get(&g_frame_count);
    }
    if (buf_requests != NULL) {
        *buf_requests = (uint32_t)atomic_get(&g_buf_requests);
    }
    if (buf_rejects != NULL) {
        *buf_rejects = (uint32_t)atomic_get(&g_buf_rejects);
    }
    if (zero_packets != NULL) {
        *zero_packets = (uint32_t)atomic_get(&g_zero_packets);
    }
    if (invalid_packets != NULL) *invalid_packets = (uint32_t)atomic_get(&g_invalid_packets);
    if (odd_byte_packets != NULL) *odd_byte_packets = (uint32_t)atomic_get(&g_odd_byte_packets);
    if (checksum != NULL) *checksum = (uint32_t)atomic_get(&g_checksum);
}


int usb_uac2_device_init(uint32_t sample_rate, uint8_t channels, uint8_t bits_per_sample)
{
    int ret;

    if (sample_rate != 48000U || channels != 2U || bits_per_sample != 16U) {
        return -EINVAL;
    }

    ret = usbd_add_descriptor(&uac2_device, &uac2_lang);
    if (ret != 0) {
        return ret;
    }
    ret = usbd_add_descriptor(&uac2_device, &uac2_mfr);
    if (ret != 0) {
        return ret;
    }
    ret = usbd_add_descriptor(&uac2_device, &uac2_product);
    if (ret != 0) {
        return ret;
    }
    ret = usbd_add_configuration(&uac2_device, USBD_SPEED_FS, &uac2_config);
    if (ret != 0) {
        return ret;
    }
    ret = usbd_register_all_classes(&uac2_device, USBD_SPEED_FS, 1, uac2_blocklist);
    if (ret != 0) {
        return ret;
    }
    usbd_device_set_code_triple(&uac2_device, USBD_SPEED_FS,
                                USB_BCC_MISCELLANEOUS, 0x02, 0x01);
    usbd_uac2_set_ops(DEVICE_DT_GET(DT_NODELABEL(uac2_audio)), &uac2_ops, NULL);
    ret = usbd_init(&uac2_device);
    if (ret != 0) {
        return ret;
    }
    return usbd_enable(&uac2_device);
}

