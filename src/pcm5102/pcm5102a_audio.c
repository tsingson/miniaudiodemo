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
/* 128 frames matches what every real-time caller (UAC2, play_mcu,
 * main_pcm5102_st7735s) already feeds per call; a smaller block shrinks the
 * accumulator below 4x, and the freed RAM is reinvested into a deeper
 * PCM_SLAB_BLOCK_COUNT for more underrun tolerance at the same total budget
 * (main_pcm5102a_test.c still works via pcm5102a_audio_output_stage()'s
 * general chunking loop, even though it feeds 512-frame blocks).
 */
#define PCM_BLOCK_FRAMES 128U
#define PCM_BYTES_PER_FRAME ((PCM_WORD_SIZE_BITS / 8U) * PCM_CHANNELS)
#define PCM_BLOCK_SIZE (PCM_BLOCK_FRAMES * PCM_BYTES_PER_FRAME)
#define PCM_SLAB_BLOCK_COUNT 20U
/* Must stay below CONFIG_I2S_STM32_TX_BLOCK_COUNT (12, see prj.conf): priming
 * calls i2s_write() before I2S_TRIGGER_START, so nothing drains the
 * underlying i2s_stm32 driver's own TX queue yet. Priming past its depth
 * deadlocks every write from block 13 onward (blocks for the full
 * i2s_config.timeout, then fails with -EAGAIN) since DMA never starts to
 * free up space -- this silently prevented playback from ever starting.
 */
#define PCM_PRIME_BLOCKS 8U
#define PCM_I2S_NODE DT_CHOSEN(miniaudio_i2s_tx)

#if !DT_NODE_HAS_STATUS(PCM_I2S_NODE, okay)
#error "chosen miniaudio,i2s-tx is missing"
#endif

/* DMA double buffering configuration */
#define DMA_DOUBLE_BUFFER_COUNT 2U
#define DMA_BUFFER_SIZE (PCM_BLOCK_SIZE * DMA_DOUBLE_BUFFER_COUNT)

/* DMA double buffer state */
static struct {
    void *buffers[DMA_DOUBLE_BUFFER_COUNT];
    bool buffer_in_use[DMA_DOUBLE_BUFFER_COUNT];
    uint32_t current_buffer;
    bool dma_active;
} dma_double_buffer_state = {0};

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
static uint32_t g_slab_used_min = PCM_SLAB_BLOCK_COUNT;
static uint32_t g_slab_used_max;

static void reset_stream_state(void)
{
    g_started = false;
    g_queued_before_start = 0U;
}

static void track_slab_range(void)
{
    uint32_t used = pcm5102a_audio_get_slab_used();

    if (used < g_slab_used_min) {
        g_slab_used_min = used;
    }
    if (used > g_slab_used_max) {
        g_slab_used_max = used;
    }
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

static int init_dma_double_buffers(void)
{
    int ret;

    /* Allocate DMA buffers for double buffering */
    for (uint32_t i = 0U; i < DMA_DOUBLE_BUFFER_COUNT; ++i) {
        ret = k_mem_slab_alloc(&pcm_tx_slab, &dma_double_buffer_state.buffers[i], K_FOREVER);
        if (ret != 0) {
            LOG_ERR("Failed to allocate DMA buffer %u: %d", i, ret);
            return ret;
        }
        dma_double_buffer_state.buffer_in_use[i] = false;
    }

    dma_double_buffer_state.current_buffer = 0;
    dma_double_buffer_state.dma_active = false;
    LOG_INF("DMA double buffers initialized: %u buffers of size %u bytes", DMA_DOUBLE_BUFFER_COUNT, PCM_BLOCK_SIZE);
    return 0;
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

    ret = init_dma_double_buffers();
    if (ret != 0) {
        LOG_ERR("Failed to initialize DMA double buffers: %d", ret);
        return ret;
    }

    g_ready = true;
    reset_stream_state();
    LOG_INF("PCM5102A I2S ready: 48k stereo PCM16 DIN/BCK/LRCK");
    return 0;
}

static void write_float_unlocked(const float *interleaved, uint32_t frames)
{
    int ret;

    if (frames > PCM_BLOCK_FRAMES) {
        frames = PCM_BLOCK_FRAMES;
    }
    /* DMA double buffering implementation */
    uint32_t current_buffer = dma_double_buffer_state.current_buffer;
    void *tx_block = dma_double_buffer_state.buffers[current_buffer];

    /* Prepare audio data in the DMA buffer */
    int16_t *tx_block_int16 = (int16_t *)tx_block;
    for (uint32_t i = 0U; i < frames * PCM_CHANNELS; ++i) {
        float value = interleaved[i];
        if (value > 1.0f) value = 1.0f;
        if (value < -1.0f) value = -1.0f;
        tx_block_int16[i] = (int16_t)lrintf(value * 32767.0f);
    }
    for (uint32_t i = frames * PCM_CHANNELS; i < PCM_BLOCK_FRAMES * PCM_CHANNELS; ++i) {
        tx_block_int16[i] = 0;
    }

    /* Mark buffer as in use and switch to next buffer */
    dma_double_buffer_state.buffer_in_use[current_buffer] = true;
    dma_double_buffer_state.current_buffer = (current_buffer + 1) % DMA_DOUBLE_BUFFER_COUNT;

    /* Use DMA to transfer data to I2S - this is non-blocking */
    ret = i2s_write(g_i2s_dev, tx_block, PCM_BLOCK_SIZE);
    if (ret != 0) {
        g_write_errors++;
        LOG_WRN("PCM5102A I2S write failed: %d", ret);
        dma_double_buffer_state.buffer_in_use[current_buffer] = false;
        if (ret == -EIO) {
            recover_i2s("write-eio");
        }
        return;
    }

    track_slab_range();
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
    uint32_t offset = 0U;
    bool planar;

    if (block == NULL || block->channels != 2U || block->sampleRate != PCM_SAMPLE_RATE) {
        return -EINVAL;
    }
    (void)ctx;

    planar = (block->layout == PLAY_BUFFER_LAYOUT_PLANAR);
    if (planar) {
        if (block->planar == NULL || block->planar[0] == NULL || block->planar[1] == NULL) {
            return -EINVAL;
        }
    } else if (block->layout == PLAY_BUFFER_LAYOUT_INTERLEAVED) {
        if (block->interleaved == NULL) {
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    /* Chunk into PCM_BLOCK_FRAMES pieces rather than requiring frameCount to
     * fit in one go: callers feed different granularities (UAC2/play_mcu use
     * 128 frames, main_pcm5102a_test.c uses 512), and this stays correct
     * either way instead of assuming a single accumulate-then-flush.
     */
    frames = block->frameCount;
    while (offset < frames) {
        uint32_t space = PCM_BLOCK_FRAMES - g_output_accumulated_frames;
        uint32_t chunk = (frames - offset) < space ? (frames - offset) : space;

        if (planar) {
            for (uint32_t i = 0U; i < chunk; ++i) {
                g_output_accumulator[(g_output_accumulated_frames + i) * PCM_CHANNELS] =
                    block->planar[0][offset + i];
                g_output_accumulator[(g_output_accumulated_frames + i) * PCM_CHANNELS + 1U] =
                    block->planar[1][offset + i];
            }
        } else {
            memcpy(&g_output_accumulator[g_output_accumulated_frames * PCM_CHANNELS],
                   &block->interleaved[offset * PCM_CHANNELS],
                   (size_t)chunk * PCM_CHANNELS * sizeof(float));
        }

        g_output_accumulated_frames += chunk;
        offset += chunk;
        if (g_output_accumulated_frames == PCM_BLOCK_FRAMES) {
            pcm5102a_audio_write_float(g_output_accumulator, PCM_BLOCK_FRAMES);
            g_output_accumulated_frames = 0U;
        }
    }
    return 0;
}

void pcm5102a_audio_get_stats(uint32_t *write_blocks, uint32_t *write_errors)
{
    if (write_blocks != NULL) *write_blocks = g_write_blocks;
    if (write_errors != NULL) *write_errors = g_write_errors;
}

uint32_t pcm5102a_audio_get_slab_used(void)
{
    return (uint32_t)k_mem_slab_num_used_get(&pcm_tx_slab);
}

uint32_t pcm5102a_audio_get_slab_capacity(void)
{
    return (uint32_t)PCM_SLAB_BLOCK_COUNT;
}

void pcm5102a_audio_sample_slab_range(uint32_t *min_used, uint32_t *max_used)
{
    if (min_used != NULL) *min_used = g_slab_used_min;
    if (max_used != NULL) *max_used = g_slab_used_max;
    g_slab_used_min = PCM_SLAB_BLOCK_COUNT;
    g_slab_used_max = 0U;
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
