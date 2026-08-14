#include "pcm5102a_audio.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pcm5102a_audio, LOG_LEVEL_INF);

#define PCM_SAMPLE_RATE 48000U
#define PCM_CHANNELS 2U
#define PCM_WORD_SIZE_BITS 16U
#define PCM_BLOCK_FRAMES 512U
#define PCM_BYTES_PER_FRAME ((PCM_WORD_SIZE_BITS / 8U) * PCM_CHANNELS)
#define PCM_BLOCK_SIZE (PCM_BLOCK_FRAMES * PCM_BYTES_PER_FRAME)
#define PCM_SLAB_BLOCK_COUNT 4U
#define PCM_PRIME_BLOCKS 4U
#define PCM_I2S_NODE DT_CHOSEN(miniaudio_i2s_tx)

#if !DT_NODE_HAS_STATUS(PCM_I2S_NODE, okay)
#error "chosen miniaudio,i2s-tx is missing"
#endif

K_MEM_SLAB_DEFINE_STATIC(pcm_tx_slab, PCM_BLOCK_SIZE, PCM_SLAB_BLOCK_COUNT, 4);

static const struct device *g_i2s_dev = DEVICE_DT_GET(PCM_I2S_NODE);
static struct k_mutex g_writer_mutex;
static bool g_mutex_ready;
static bool g_ready;
static bool g_started;
static uint32_t g_queued_before_start;
static uint32_t g_write_blocks;
static uint32_t g_write_errors;
static float g_output_accumulator[PCM_BLOCK_FRAMES * PCM_CHANNELS];
static uint32_t g_output_accumulated_frames;

static void reset_stream_state(void)
{
    g_started = false;
    g_queued_before_start = 0U;
}

static int configure_i2s(void)
{
    struct i2s_config config = {
        .word_size = PCM_WORD_SIZE_BITS,
        .channels = PCM_CHANNELS,
        .format = I2S_FMT_DATA_FORMAT_I2S | I2S_FMT_CLK_NF_NB,
        .options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
        .frame_clk_freq = PCM_SAMPLE_RATE,
        .mem_slab = &pcm_tx_slab,
        .block_size = PCM_BLOCK_SIZE,
        .timeout = 1000,
    };

    return i2s_configure(g_i2s_dev, I2S_DIR_TX, &config);
}

static void recover_i2s(const char *reason)
{
    int prepare = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
    int drop = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
    int config = configure_i2s();

    reset_stream_state();
    LOG_WRN("PCM5102A I2S recover (%s): prepare=%d drop=%d cfg=%d",
            reason, prepare, drop, config);
    if (config != 0) {
        g_ready = false;
    }
}

int pcm5102a_audio_init(void)
{
    int ret;

    if (!g_mutex_ready) {
        k_mutex_init(&g_writer_mutex);
        g_mutex_ready = true;
    }
    if (g_ready) {
        return 0;
    }
    if (!device_is_ready(g_i2s_dev)) {
        LOG_ERR("PCM5102A I2S device not ready");
        return -ENODEV;
    }

    ret = configure_i2s();
    if (ret != 0) {
        LOG_ERR("PCM5102A I2S configure failed: %d", ret);
        return ret;
    }

    g_ready = true;
    reset_stream_state();
    LOG_INF("PCM5102A I2S ready: 48k stereo PCM16 DIN/BCK/LRCK");
    return 0;
}

static void write_float_unlocked(const float *interleaved, uint32_t frames)
{
    int16_t *tx_block;
    int ret;

    if (frames > PCM_BLOCK_FRAMES) {
        frames = PCM_BLOCK_FRAMES;
    }
    /* Bounded wait: this runs on the UAC2/UDC callback path, so an indefinite
     * K_FOREVER here would stall USB packet reception whenever I2S/DMA drains
     * slower than the incoming stream, causing audible glitches synced to
     * playback content. Drop the block instead of blocking the USB thread.
     */
    ret = k_mem_slab_alloc(&pcm_tx_slab, (void **)&tx_block, K_MSEC(2));
    if (ret != 0) {
        g_write_errors++;
        LOG_WRN("PCM5102A TX slab allocation failed: %d", ret);
        return;
    }

    for (uint32_t i = 0U; i < frames * PCM_CHANNELS; ++i) {
        float value = interleaved[i];
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        tx_block[i] = (int16_t)lrintf(value * 32767.0f);
    }
    for (uint32_t i = frames * PCM_CHANNELS; i < PCM_BLOCK_FRAMES * PCM_CHANNELS; ++i) {
        tx_block[i] = 0;
    }

    ret = i2s_write(g_i2s_dev, tx_block, PCM_BLOCK_SIZE);
    if (ret != 0) {
        g_write_errors++;
        LOG_WRN("PCM5102A I2S write failed: %d", ret);
        k_mem_slab_free(&pcm_tx_slab, tx_block);
        if (ret == -EIO) {
            recover_i2s("write-eio");
        }
        return;
    }

    g_write_blocks++;
    if (!g_started) {
        g_queued_before_start++;
        if (g_queued_before_start < PCM_PRIME_BLOCKS) {
            return;
        }
        ret = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
        if (ret != 0) {
            g_write_errors++;
            LOG_WRN("PCM5102A I2S start failed: %d", ret);
            recover_i2s("start-fail");
            return;
        }
        g_started = true;
        g_queued_before_start = 0U;
        LOG_INF("PCM5102A audio stream started");
    }
}

void pcm5102a_audio_write_float(const float *interleaved, uint32_t frames)
{
    if (interleaved == NULL || frames == 0U) {
        return;
    }
    if (!g_mutex_ready && pcm5102a_audio_init() != 0) {
        return;
    }
    k_mutex_lock(&g_writer_mutex, K_FOREVER);
    if (!g_ready && pcm5102a_audio_init() != 0) {
        k_mutex_unlock(&g_writer_mutex);
        return;
    }
    write_float_unlocked(interleaved, frames);
    k_mutex_unlock(&g_writer_mutex);
}

void pcm5102a_audio_write_pcm16(const int16_t *interleaved, uint32_t frames)
{
    float block[PCM_BLOCK_FRAMES * PCM_CHANNELS];

    if (interleaved == NULL || frames == 0U) {
        return;
    }
    if (frames > PCM_BLOCK_FRAMES) {
        frames = PCM_BLOCK_FRAMES;
    }
    for (uint32_t i = 0U; i < frames * PCM_CHANNELS; ++i) {
        block[i] = (float)interleaved[i] / 32768.0f;
    }
    pcm5102a_audio_write_float(block, frames);
}

int pcm5102a_audio_output_stage(void *ctx, play_frame_block *block)
{
    uint32_t frames;

    if (block == NULL || block->channels != 2U || block->sampleRate != PCM_SAMPLE_RATE) {
        return -EINVAL;
    }
    (void)ctx;
    if (block->frameCount > PCM_BLOCK_FRAMES || block->planar == NULL ||
        block->planar[0] == NULL || block->planar[1] == NULL) {
        if (block->layout != PLAY_BUFFER_LAYOUT_INTERLEAVED ||
            block->frameCount > PCM_BLOCK_FRAMES || block->interleaved == NULL) {
            return -EINVAL;
        }
    }

    frames = block->frameCount;
    if (block->layout == PLAY_BUFFER_LAYOUT_INTERLEAVED) {
        memcpy(&g_output_accumulator[g_output_accumulated_frames * PCM_CHANNELS],
               block->interleaved, frames * PCM_CHANNELS * sizeof(float));
    } else {
        for (uint32_t frame = 0U; frame < frames; ++frame) {
            g_output_accumulator[(g_output_accumulated_frames + frame) * PCM_CHANNELS] =
                block->planar[0][frame];
            g_output_accumulator[(g_output_accumulated_frames + frame) * PCM_CHANNELS + 1U] =
                block->planar[1][frame];
        }
    }

    g_output_accumulated_frames += frames;
    if (g_output_accumulated_frames == PCM_BLOCK_FRAMES) {
        pcm5102a_audio_write_float(g_output_accumulator, PCM_BLOCK_FRAMES);
        g_output_accumulated_frames = 0U;
    }
    return 0;
}

void pcm5102a_audio_get_stats(uint32_t *write_blocks, uint32_t *write_errors)
{
    if (write_blocks != NULL) *write_blocks = g_write_blocks;
    if (write_errors != NULL) *write_errors = g_write_errors;
}

/* Compatibility symbols for existing playback entrypoints. */
int play_mcu_pcm5102a_init(void)
{
    return pcm5102a_audio_init();
}

void play_mcu_write_frames(const float *interleaved, uint32_t frames)
{
    pcm5102a_audio_write_float(interleaved, frames);
}
