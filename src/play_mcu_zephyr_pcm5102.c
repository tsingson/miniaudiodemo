#include "play_dsp_common.h"

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
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
#define PLAY_I2S_SLAB_BLOCK_COUNT 12
#define PLAY_I2S_PRIME_BLOCKS 3U

#define PLAY_I2S_NODE DT_CHOSEN(miniaudio_i2s_tx)

#if !DT_NODE_HAS_STATUS(PLAY_I2S_NODE, okay)
#error "chosen miniaudio,i2s-tx is missing; check boards/stm32f401rct6.overlay"
#endif

K_MEM_SLAB_DEFINE_STATIC(play_i2s_tx_slab, PLAY_BLOCK_SIZE, PLAY_I2S_SLAB_BLOCK_COUNT, 4);

static const struct device *g_i2s_dev = DEVICE_DT_GET(PLAY_I2S_NODE);

static bool g_i2s_ready;
static bool g_i2s_started;
static bool g_lcd_ready;
static bool g_lcd_init_attempted;
static uint32_t g_heartbeat;
static uint32_t g_i2s_write_fail_count;
static uint32_t g_tx_queued_before_start;

static int play_i2s_configure_current_format(void);

static void play_i2s_reset_tx_state(void)
{
	g_i2s_started = false;
	g_tx_queued_before_start = 0U;
}

static void play_i2s_recover(const char *reason)
{
	int ret_prepare;
	int ret_drop;
	int ret_cfg;
	char msg[32];

	ret_prepare = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
	ret_drop = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	/* Reconfigure after DROP so the peripheral/DMA state is deterministic for next START. */
	ret_cfg = play_i2s_configure_current_format();
	play_i2s_reset_tx_state();

	LOG_WRN("I2S recover (%s): prepare=%d drop=%d cfg=%d", reason, ret_prepare, ret_drop, ret_cfg);
	if (g_lcd_ready) {
		(void)snprintf(msg, sizeof(msg), "I2S rec p%d d%d", ret_prepare, ret_drop);
		st7735s_log_display_line(msg);
	}

	if (ret_cfg != 0) {
		g_i2s_ready = false;
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S rec cfg fail");
		}
	}
}

static int play_i2s_configure_current_format(void)
{
	struct i2s_config cfg = {
		.word_size = PLAY_WORD_SIZE_BITS,
		.channels = PLAY_CHANNELS,
		.format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB,
		.options = I2S_OPT_BIT_CLK_CONTROLLER |
				   I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = PLAY_SAMPLE_RATE,
		.mem_slab = &play_i2s_tx_slab,
		.block_size = PLAY_BLOCK_SIZE,
		.timeout = 1000,
	};

	return i2s_configure(g_i2s_dev, I2S_DIR_TX, &cfg);
}
static int play_i2s_init(void)
{
	int ret;

	if (!device_is_ready(g_i2s_dev)) {
		LOG_ERR("I2S device not ready");
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S not ready");
		}
		return -ENODEV;
	}

	if (!g_lcd_ready && !g_lcd_init_attempted) {
		g_lcd_init_attempted = true;
		LOG_INF("Attempting ST7735S logger init");
		if (st7735s_log_display_init() == 0) {
			g_lcd_ready = true;
			LOG_INF("ST7735S logger init OK");
			st7735s_log_display_line("System boot");
		} else {
			LOG_WRN("ST7735S logger init failed");
		}
	}

	ret = play_i2s_configure_current_format();
	if (ret != 0) {
		LOG_ERR("i2s_configure failed: %d", ret);
		if (g_lcd_ready) {
			st7735s_log_display_line("I2S config failed");
		}
		return ret;
	}

	g_i2s_ready = true;
	play_i2s_reset_tx_state();
	LOG_INF("PCM5102A I2S TX initialized @ %u Hz", PLAY_SAMPLE_RATE);
	LOG_INF("I2S running with continuous BCLK/LRCLK");
	LOG_INF("I2S block size=%u bytes, slab blocks=%u", (unsigned int)PLAY_BLOCK_SIZE, (unsigned int)PLAY_I2S_SLAB_BLOCK_COUNT);
	if (g_lcd_ready) {
		st7735s_log_display_line("PCM5102A ready 48k");
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
			st7735s_log_display_line("Running...");
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
			st7735s_log_display_line("I2S TX buffer short");
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
		char msg[32];
		++g_i2s_write_fail_count;
		LOG_WRN("i2s_write failed: %d", ret);
		if (g_lcd_ready) {
			(void)snprintf(msg, sizeof(msg), "I2S wr fail %d", ret);
			st7735s_log_display_line(msg);
		}
		k_mem_slab_free(&play_i2s_tx_slab, (void *)tx_block);

		if (ret == -EAGAIN || ret == -EBUSY || ret == -ENOMEM) {
			/* TX queue temporary pressure: keep stream and retry on next block. */
			k_sleep(K_MSEC(1));
			return;
		}

		if (ret == -EIO) {
			/* For STM32 driver, EIO here means TX state left READY/RUNNING (often ERROR due underrun). */
			play_i2s_recover("write-eio");
			return;
		}

		play_i2s_recover("write-other");
		return;
	}

	if (!g_i2s_started) {
		++g_tx_queued_before_start;
		if (g_tx_queued_before_start < PLAY_I2S_PRIME_BLOCKS) {
			return;
		}

		ret = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
		if (ret != 0) {
			LOG_WRN("i2s start failed: %d", ret);
			if (g_lcd_ready) {
				st7735s_log_display_line("I2S start failed");
			}
			play_i2s_recover("start-fail");
			return;
		}
		g_i2s_started = true;
		g_tx_queued_before_start = 0U;
		if (g_lcd_ready) {
			st7735s_log_display_line("Audio stream started");
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
