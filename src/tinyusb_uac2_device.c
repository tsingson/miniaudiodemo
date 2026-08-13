#include <string.h>

#include "tusb.h"
#include "tinyusb_uac2_descriptors.h"

#define TINYUSB_UAC2_SAMPLE_RATE 48000U
#define TINYUSB_UAC2_PRODUCT_ID 0x0203U

static uint8_t g_mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1U];
static int16_t g_volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1U];

static tusb_desc_device_t const g_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0xCAFE,
    .idProduct = TINYUSB_UAC2_PRODUCT_ID,
    .bcdDevice = 0x0101,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define TINYUSB_UAC2_CONFIG_LEN (TUD_CONFIG_DESC_LEN + TINYUSB_UAC2_STEREO_SPEAKER_DESC_LEN)

static uint8_t const g_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TINYUSB_UAC2_CONFIG_LEN, 0x80, 100),
    TINYUSB_UAC2_STEREO_SPEAKER_DESCRIPTOR(0, 4,
                                            CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX,
                                            CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX,
                                            0x01, CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX,
                                            0x81, 4),
};

TU_VERIFY_STATIC(sizeof(g_configuration_descriptor) == TINYUSB_UAC2_CONFIG_LEN,
                 "Invalid TinyUSB UAC2 descriptor size");

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&g_device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return g_configuration_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t descriptor[32];
    static uint8_t const language_id[] = {0x09, 0x04};
    static char const *const strings[] = {
        "miniaudiodemo",
        "PCM5102A UAC2",
        "0002",
        "UAC2 Audio",
    };
    size_t length;

    (void)langid;
    if (index == 0U) {
        memcpy(&descriptor[1], language_id, sizeof(language_id));
        length = 1U;
    } else {
        if (index > TU_ARRAY_SIZE(strings)) {
            return NULL;
        }
        length = strlen(strings[index - 1U]);
        if (length > TU_ARRAY_SIZE(descriptor) - 1U) {
            length = TU_ARRAY_SIZE(descriptor) - 1U;
        }
        for (size_t position = 0U; position < length; ++position) {
            descriptor[position + 1U] = strings[index - 1U][position];
        }
    }
    descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8U) | (2U * length + 2U));
    return descriptor;
}

static bool clock_get_request(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t selector = TU_U16_HIGH(request->wValue);

    if (selector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_4_t frequency = {.bCur = (int32_t)tu_htole32(TINYUSB_UAC2_SAMPLE_RATE)};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &frequency,
                                                           sizeof(frequency));
    }
    if (selector == AUDIO20_CS_CTRL_SAM_FREQ && request->bRequest == AUDIO20_CS_REQ_RANGE) {
        audio20_control_range_4_n_t(1) range = {
            .wNumSubRanges = tu_htole16(1),
            .subrange[0] = {TINYUSB_UAC2_SAMPLE_RATE, TINYUSB_UAC2_SAMPLE_RATE, 0},
        };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &range, sizeof(range));
    }
    if (selector == AUDIO20_CS_CTRL_CLK_VALID && request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t valid = {.bCur = 1};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &valid, sizeof(valid));
    }
    return false;
}

static bool feature_get_request(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t selector = TU_U16_HIGH(request->wValue);
    uint8_t channel = TU_U16_LOW(request->wValue);

    if (channel > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) {
        return false;
    }
    if (selector == AUDIO20_FU_CTRL_MUTE && request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_1_t mute = {.bCur = g_mute[channel]};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &mute, sizeof(mute));
    }
    if (selector == AUDIO20_FU_CTRL_VOLUME && request->bRequest == AUDIO20_CS_REQ_CUR) {
        audio20_control_cur_2_t volume = {.bCur = tu_htole16(g_volume[channel])};
        return tud_audio_buffer_and_schedule_control_xfer(rhport, request, &volume, sizeof(volume));
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    uint8_t entity = TU_U16_HIGH(request->wIndex);

    if (entity == TINYUSB_UAC2_ENTITY_CLOCK) {
        return clock_get_request(rhport, request);
    }
    if (entity == TINYUSB_UAC2_ENTITY_FEATURE_UNIT) {
        return feature_get_request(rhport, request);
    }
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *request,
                                 uint8_t *buffer)
{
    uint8_t entity = TU_U16_HIGH(request->wIndex);
    uint8_t selector = TU_U16_HIGH(request->wValue);
    uint8_t channel = TU_U16_LOW(request->wValue);

    (void)rhport;
    if (entity != TINYUSB_UAC2_ENTITY_FEATURE_UNIT || request->bRequest != AUDIO20_CS_REQ_CUR ||
        channel > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) {
        return false;
    }
    if (selector == AUDIO20_FU_CTRL_MUTE && request->wLength == sizeof(audio20_control_cur_1_t)) {
        g_mute[channel] = ((audio20_control_cur_1_t const *)buffer)->bCur;
        return true;
    }
    if (selector == AUDIO20_FU_CTRL_VOLUME && request->wLength == sizeof(audio20_control_cur_2_t)) {
        g_volume[channel] = ((audio20_control_cur_2_t const *)buffer)->bCur;
        return true;
    }
    return false;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return true;
}

bool tud_audio_set_itf_close_ep_cb(uint8_t rhport, tusb_control_request_t const *request)
{
    (void)rhport;
    (void)request;
    return true;
}

void tud_audio_feedback_params_cb(uint8_t function_id, uint8_t alternate_setting,
                                  audio_feedback_params_t *parameters)
{
    (void)function_id;
    (void)alternate_setting;
    parameters->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    parameters->sample_freq = TINYUSB_UAC2_SAMPLE_RATE;
}