#include "play_dsp_common.h"

#include <math.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "st7735s_log_display.h"

LOG_MODULE_REGISTER(play_mcu_zephyr, LOG_LEVEL_INF);

#define PLAY_SAMPLE_RATE 48000U
#define PLAY_CHANNELS 2U
#define PLAY_WORD_SIZE_BITS 16U
#define PLAY_BLOCK_FRAMES 128U
#define PLAY_BYTES_PER_FRAME ((PLAY_WORD_SIZE_BITS / 8U) * PLAY_CHANNELS)
#define PLAY_BLOCK_SIZE (PLAY_BLOCK_FRAMES * PLAY_BYTES_PER_FRAME)
#define PLAY_I2S_SLAB_BLOCK_COUNT 8

#define PLAY_I2S_NODE DT_CHOSEN(miniaudio_i2s_tx)
#define PCM5102A_CTRL_NODE DT_NODELABEL(pcm5102a_ctrl)

#if !DT_NODE_HAS_STATUS(PLAY_I2S_NODE, okay)
#error "chosen miniaudio,i2s-tx is missing; check boards/stm32f401rct6.overlay"
#endif

K_MEM_SLAB_DEFINE_STATIC(play_i2s_tx_slab, PLAY_BLOCK_SIZE, PLAY_I2S_SLAB_BLOCK_COUNT, 4);

static const struct device *g_i2s_dev = DEVICE_DT_GET(PLAY_I2S_NODE);

static const struct gpio_dt_spec g_flt = GPIO_DT_SPEC_GET_OR(PCM5102A_CTRL_NODE, flt_gpios, {0});
static const struct gpio_dt_spec g_demp = GPIO_DT_SPEC_GET_OR(PCM5102A_CTRL_NODE, demp_gpios, {0});
static const struct gpio_dt_spec g_xsmt = GPIO_DT_SPEC_GET_OR(PCM5102A_CTRL_NODE, xsmt_gpios, {0});
static const struct gpio_dt_spec g_fmt = GPIO_DT_SPEC_GET_OR(PCM5102A_CTRL_NODE, fmt_gpios, {0});
static const struct gpio_dt_spec g_lineout_en = GPIO_DT_SPEC_GET_OR(PCM5102A_CTRL_NODE, lineout_en_gpios, {0});

static bool g_i2s_ready;
static bool g_i2s_started;
static bool g_lcd_ready;
static bool g_lcd_init_attempted;
static uint32_t g_heartbeat;

static int pcm5102a_pin_init_one(const struct gpio_dt_spec *spec, int initial, const char *name)
{
	if ((spec == NULL) || (spec->port == NULL)) {
		LOG_INF("PCM5102A pin %s not configured", name);
		return 0;
	}
	if (!device_is_ready(spec->port)) {
		LOG_ERR("PCM5102A pin %s gpio port not ready", name);
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE | (initial ? GPIO_OUTPUT_ACTIVE : 0));
	if (ret == 0) {
		LOG_INF("PCM5102A pin %s configured initial=%d", name, initial);
	} else {
		LOG_ERR("PCM5102A pin %s configure failed: %d", name, ret);
	}
	return ret;
}

static void pcm5102a_control_pins_init(void)
{
	(void)pcm5102a_pin_init_one(&g_flt, 0, "FLT");
	(void)pcm5102a_pin_init_one(&g_demp, 0, "DEMP");
	(void)pcm5102a_pin_init_one(&g_fmt, 0, "FMT");
	(void)pcm5102a_pin_init_one(&g_lineout_en, 1, "LINEOUT_EN");
	(void)pcm5102a_pin_init_one(&g_xsmt, 1, "XSMT");
}

static int play_i2s_init(void)
{
	struct i2s_config cfg = {
		.word_size = PLAY_WORD_SIZE_BITS,
		.channels = PLAY_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB,
		.options = I2S_OPT_BIT_CLK_GATED |
				   I2S_OPT_BIT_CLK_CONTROLLER |
				   I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = PLAY_SAMPLE_RATE,
		.mem_slab = &play_i2s_tx_slab,
		.block_size = PLAY_BLOCK_SIZE,
		.timeout = 1000,
	};
	int ret;

	if (!device_is_ready(g_i2s_dev)) {
		LOG_ERR("I2S device not ready");
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S设备未就绪");
		}
		return -ENODEV;
	}

	if (!g_lcd_ready && !g_lcd_init_attempted) {
		g_lcd_init_attempted = true;
		LOG_INF("Attempting ST7735S logger init");
		if (st7735s_log_display_init() == 0) {
			g_lcd_ready = true;
			LOG_INF("ST7735S logger init OK");
			st7735s_log_display_line("系统启动 / Boot");
		} else {
			LOG_WRN("ST7735S logger init failed");
		}
	}

	pcm5102a_control_pins_init();

	ret = i2s_configure(g_i2s_dev, I2S_DIR_TX, &cfg);
	if (ret != 0) {
		LOG_ERR("i2s_configure failed: %d", ret);
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S配置失败");
		}
		return ret;
	}

	g_i2s_ready = true;
	g_i2s_started = false;
	LOG_INF("PCM5102A I2S TX initialized @ %u Hz", PLAY_SAMPLE_RATE);
	LOG_INF("I2S block size=%u bytes, slab blocks=%u", (unsigned int)PLAY_BLOCK_SIZE, (unsigned int)PLAY_I2S_SLAB_BLOCK_COUNT);
	if (g_lcd_ready) {
		st7735s_log_display_line("PCM5102A就绪 48kHz");
	}
	return 0;
}

uint32_t play_mcu_read_frames(float *interleavedOut, uint32_t maxFrames)
{
	(void)interleavedOut;
	(void)maxFrames;

	if (!g_i2s_ready) {
		(void)play_i2s_init();
	}

	if (g_lcd_ready) {
		++g_heartbeat;
		if ((g_heartbeat % 1000U) == 0U) {
			st7735s_log_display_line("运行中...");
		}
	}

	k_sleep(K_MSEC(1));
	return 0;
}

void play_mcu_write_frames(const float *interleavedIn, uint32_t frameCount)
{
	int16_t *tx_block;
	size_t bytes;
	int ret;

	if ((interleavedIn == NULL) || (frameCount == 0U)) {
		return;
	}

	if (!g_i2s_ready) {
		ret = play_i2s_init();
		if (ret != 0) {
			return;
		}
	}

	if (frameCount > PLAY_BLOCK_FRAMES) {
		frameCount = PLAY_BLOCK_FRAMES;
	}

	ret = k_mem_slab_alloc(&play_i2s_tx_slab, (void **)&tx_block, K_MSEC(100));
	if (ret != 0) {
		LOG_WRN("No TX slab block: %d", ret);
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S发送缓冲不足");
		}
		return;
	}

	for (uint32_t i = 0; i < frameCount * PLAY_CHANNELS; ++i) {
		float v = interleavedIn[i];
		if (v > 1.0f) {
			v = 1.0f;
		} else if (v < -1.0f) {
			v = -1.0f;
		}
		tx_block[i] = (int16_t)lrintf(v * 32767.0f);
	}

	for (uint32_t i = frameCount * PLAY_CHANNELS; i < PLAY_BLOCK_FRAMES * PLAY_CHANNELS; ++i) {
		tx_block[i] = 0;
	}

	bytes = PLAY_BLOCK_SIZE;
	ret = i2s_write(g_i2s_dev, tx_block, bytes);
	if (ret != 0) {
		LOG_WRN("i2s_write failed: %d", ret);
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S发送失败");
		}
		k_mem_slab_free(&play_i2s_tx_slab, (void *)tx_block);
		(void)i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		g_i2s_started = false;
		return;
	}

	if (!g_i2s_started) {
		ret = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		if (ret != 0) {
			LOG_WRN("i2s start failed: %d", ret);
			if (g_lcd_ready) {
				st7735s_log_display_line("I2S启动失败");
			}
			(void)i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
			g_i2s_started = false;
			return;
		}
		g_i2s_started = true;
		if (g_lcd_ready) {
			st7735s_log_display_line("音频流已启动");
		}
	}
}

void play_mcu_poll_eq(float *gainsOut, float *qsOut, int maxBands)
{
	if ((gainsOut == NULL) || (qsOut == NULL) || (maxBands <= 0)) {
		return;
	}

	for (int i = 0; i < maxBands; ++i) {
		gainsOut[i] = 0.0f;
		qsOut[i] = 0.707f;
	}
}
