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

#define UAC2_INPUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(uac2_input))
#define UAC2_MAX_PACKET_SIZE 200U
#define UAC2_BLOCK_FRAMES 48U
#define UAC2_CHANNELS 2U
#define UAC2_I2S_BLOCK_FRAMES 128U
#define UAC2_FS_FEEDBACK (48U << 14U)

K_MEM_SLAB_DEFINE_STATIC(uac2_rx_slab,
             ROUND_UP(UAC2_MAX_PACKET_SIZE, UDC_BUF_GRANULARITY),
             8, UDC_BUF_ALIGN);

static float pending_samples[UAC2_I2S_BLOCK_FRAMES * UAC2_CHANNELS];
static size_t pending_frames;
static atomic_t g_uac2_stream_active;
static atomic_t g_packet_count;
static atomic_t g_byte_count;
static atomic_t g_frame_count;
static atomic_t g_i2s_block_count;
static atomic_t g_buf_requests;
static atomic_t g_buf_rejects;
static atomic_t g_zero_packets;
static atomic_t g_first_packet_logged;
static atomic_t g_sof_seen;
static usb_uac2_audio_sink_fn g_audio_sink;
static void *g_audio_sink_ctx;

USBD_DEVICE_DEFINE(uac2_device,
           DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
           0x2FE3, 0x0102);
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

    frames = size / (sizeof(int16_t) * UAC2_CHANNELS);
    for (size_t i = 0U; i < frames; ++i) {
        pending_samples[pending_frames * UAC2_CHANNELS] =
            (float)pcm[i * UAC2_CHANNELS] / 32768.0f;
        pending_samples[pending_frames * UAC2_CHANNELS + 1U] =
            (float)pcm[i * UAC2_CHANNELS + 1U] / 32768.0f;
        pending_frames++;

        if (pending_frames == UAC2_I2S_BLOCK_FRAMES) {
            if (g_audio_sink != NULL &&
                g_audio_sink(g_audio_sink_ctx, pending_samples, UAC2_I2S_BLOCK_FRAMES) == 0) {
                atomic_inc(&g_i2s_block_count);
            }
            pending_frames = 0U;
        }
    }
    atomic_add(&g_frame_count, frames);
    k_mem_slab_free(&uac2_rx_slab, buf);
}

void usb_uac2_set_audio_sink(usb_uac2_audio_sink_fn sink, void *ctx)
{
    g_audio_sink = sink;
    g_audio_sink_ctx = ctx;
}

static uint32_t uac2_feedback_cb(const struct device *dev, uint8_t terminal,
                                 void *user_data)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(terminal);
    ARG_UNUSED(user_data);
    return UAC2_FS_FEEDBACK;
}

static const struct uac2_ops uac2_ops = {
    .sof_cb = uac2_sof_cb,
    .terminal_update_cb = uac2_terminal_update_cb,
    .get_recv_buf = uac2_get_recv_buf,
    .data_recv_cb = uac2_data_recv_cb,
    .feedback_cb = uac2_feedback_cb,
};

bool usb_uac2_stream_active(void)
{
    return atomic_get(&g_uac2_stream_active) != 0;
}

bool usb_uac2_first_packet_seen(void)
{
    return atomic_get(&g_first_packet_logged) != 0;
}

void usb_uac2_get_stats(uint32_t *packets, uint32_t *bytes,
                        uint32_t *frames, uint32_t *i2s_blocks,
                        uint32_t *buf_requests, uint32_t *buf_rejects,
                        uint32_t *zero_packets)
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
    if (i2s_blocks != NULL) {
        *i2s_blocks = (uint32_t)atomic_get(&g_i2s_block_count);
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

