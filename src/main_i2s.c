#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main_i2s, LOG_LEVEL_INF);

#define I2S_TX_NODE DT_CHOSEN(miniaudio_i2s_tx)


#if !DT_NODE_HAS_STATUS(I2S_TX_NODE, okay)
#error "chosen miniaudio,i2s-tx is missing; check boards/stm32f401rct6_i2s.overlay"
#endif

extern const int16_t wav_3_i2s[];
extern const uint32_t wav_3_i2s_len;
extern const uint32_t wav_3_sample_rate;
extern const uint16_t wav_3_channels;
extern const uint16_t wav_3_bits_per_sample;

static const struct device *g_i2s_dev = DEVICE_DT_GET(I2S_TX_NODE);




static int i2s_tx_configure(void)
{
    struct i2s_config cfg = {
        .word_size = 16U,
        .channels = 2U,
        .frame_clk_freq = wav_3_sample_rate,
        .mem_slab = NULL,
        .block_size = 0U,
        .timeout = 1000,
    };

    int ret = i2s_configure(g_i2s_dev, I2S_DIR_TX, &cfg);
    if (ret != 0) {
        LOG_ERR("i2s_configure failed: %d", ret);
        return ret;
    }

    ret = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
    if (ret != 0) {
        LOG_ERR("i2s prepare failed: %d", ret);
        return ret;
    }

    LOG_INF("I2S TX configured: %u Hz, %u ch, %u-bit", (unsigned int)wav_3_sample_rate,
            (unsigned int)wav_3_channels, (unsigned int)wav_3_bits_per_sample);
    return 0;
}

static void play_once(void)
{
    const size_t bytes = (size_t)wav_3_i2s_len * sizeof(wav_3_i2s[0]);
    int ret;

    ret = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
    if (ret != 0) {
        LOG_ERR("i2s start failed: %d", ret);
        return;
    }

    ret = i2s_write(g_i2s_dev, wav_3_i2s, bytes);
    if (ret < 0) {
        LOG_ERR("i2s_write failed: %d", ret);
    } else {
        LOG_INF("sent %zu bytes (%u samples) via I2S", bytes, (unsigned int)wav_3_i2s_len);
    }

    (void)i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
}

int main(void)
{
    int ret;

    if (!device_is_ready(g_i2s_dev)) {
        LOG_ERR("I2S device not ready");
        return 0;
    }

    ret = i2s_tx_configure();
    if (ret != 0) {
        return 0;
    }

    LOG_INF("PCM5102A ready; playing wav_3_i2s");

    while (1) {
        play_once();
        k_sleep(K_MSEC(250));
    }

    return 0;
}
