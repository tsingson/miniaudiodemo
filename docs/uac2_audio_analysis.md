# UAC2 音频卡顿分析报告

## 工程概览

### 架构层次
```
USB UAC2 (usb_uac2_device.c)
    ↓
Async Pipeline (play_pipeline_async.c)
    ↓
Audio Pipeline (play_audio_pipeline.c)
    ↓
DSP Processing (play_dsp_*.c)
    ↓
PCM5102A I2S Output (pcm5102a_audio.c)
```

### 关键参数
- **采样率**: 48kHz
- **声道**: 2 (立体声)
- **位深**: 16-bit
- **UAC2 Block Frames**: 128 frames/packet
- **I2S Block Frames**: 128 frames/write
- **Async Queue Depth**: 3 blocks
- **PCM Slab Blocks**: 20 blocks

---

## 卡顿原因分析

### 1. **I2S TX Slab 内存池竞争** ⚠️ 高风险

**问题位置**: `pcm5102a_audio.c:write_float_unlocked()`

```c
ret = k_mem_slab_alloc(&pcm_tx_slab, (void **)&tx_block, K_MSEC(2));
```

**问题描述**:
- TX Slab 只有 20 个 block，每个 block 128 frames × 2 channels × 4 bytes = 1024 bytes
- 总内存: 20 × 1024 = 20KB
- 在 64KB RAM 的 STM32F401 上占比 31%
- **K_MSEC(2) 的 bounded wait 在 USB callback 中是危险的**

**卡顿机制**:
```
USB packet arrives → write_float_unlocked() → slab alloc blocks for 2ms
    ↓
USB stack starved → next isochronous packet missed
    ↓
Audio glitch (卡顿)
```

**证据**:
- `pcm5102a_audio.c` 注释明确说明: "Bounded wait: this runs on the UAC2/UDC callback path, so an indefinite K_FOREVER here would stall USB packet reception"
- 但即使 2ms 也可能导致卡顿，因为 USB isochronous 传输通常每 1ms 一次

---

### 2. **Async Pipeline 队列太浅** ⚠️ 中风险

**问题位置**: `play_pipeline_async.c`

```c
#define UAC2_AUDIO_QUEUE_DEPTH 3U
```

**问题描述**:
- 只有 3 个 block 的队列深度
- 每个 block 128 frames = 2.67ms (48kHz)
- 总队列深度: 3 × 2.67ms = 8ms

**卡顿机制**:
```
Worker thread busy (I2S write) → msgq full
    ↓
UAC2 callback → k_msgq_put() fails → atomic_inc(&async->dropped)
    ↓
Audio drop (卡顿)
```

**证据**:
- `uac2_audio_stream.c` 中 stats 显示 `dropped` 字段
- `play_pipeline_async_push()` 中 `K_NO_WAIT` 不会阻塞

---

### 3. **Feedback 算法响应慢** ⚠️ 中风险

**问题位置**: `usb_uac2_device.c:uac2_feedback_cb()`

```c
#define UAC2_FB_GAIN 16
#define UAC2_FB_TARGET_PERMILLE 500
```

**问题描述**:
- 反馈调整增益只有 16，调整速度慢
- 目标填充度 500 (50%)，但 PCM5102A I2S TX slab 深度约 43ms
- **反馈信号源不匹配**:

```c
// uac2_pcm5102_app.c
static uint32_t pcm5102a_fill_permille(void *ctx)
{
    uint32_t used = pcm5102a_audio_get_slab_used();  // ← 这是 I2S TX slab
    uint32_t capacity = pcm5102a_audio_get_slab_capacity();
    return (used * 1000U) / capacity;
}
```

**卡顿机制**:
```
I2S TX slab 慢慢填满 (43ms) → feedback 响应慢 (增益 16)
    ↓
USB clock 未及时调整 → 累积误差
    ↓
最终导致 buffer underrun (卡顿)
```

---

### 4. **DSP 处理时间不确定** ⚠️ 低风险

**问题位置**: `play_dsp_common.c:play_dsp_process_planar()`

**问题描述**:
- DSP 处理包含 FFT、EQ、动态处理等
- CMSIS-DSP 优化后仍可能有波动
- `play_audio_pipeline_process()` 中没有超时保护

**证据**:
- `play_dsp_common.c` 中有 timing hook 机制
- `ANALYZER_WINDOW=32` (被缩小以适应 64KB RAM)

---

### 5. **I2S 驱动问题** ⚠️ 中风险

**问题位置**: `pcm5102a_audio.c:recover_i2s()`

```c
static void recover_i2s(const char *reason)
{
    int prepare = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
    int drop = i2s_trigger(g_i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
    int config = configure_i2s();
    // ...
}
```

**问题描述**:
- I2S recover 过程可能阻塞
- `configure_i2s()` 调用 `i2s_configure()` 可能失败
- 没有看门狗保护

---

## 改进建议

### 1. **优化 Slab Allocation** (高优先级)

**方案 A**: 使用 `K_NO_WAIT` + 预分配
```c
// 预分配所有 slab，运行时不再分配
ret = k_mem_slab_alloc(&pcm_tx_slab, (void **)&tx_block, K_NO_WAIT);
if (ret != 0) {
    // 使用备用 buffer 或丢弃 block
    g_write_errors++;
    return;
}
```

**方案 B**: 使用环形缓冲区
```c
// 替代 slab，使用固定大小环形缓冲区
static float tx_ring_buffer[PCM_SLAB_BLOCK_COUNT * PCM_BLOCK_FRAMES * PCM_CHANNELS];
static uint32_t tx_ring_head;
static uint32_t tx_ring_tail;
```

---

### 2. **增加 Async Queue Depth** (中优先级)

```c
#define UAC2_AUDIO_QUEUE_DEPTH 8U  // 从 3 增加到 8
```

**效果**: 队列深度从 8ms 增加到 21ms，容忍更多调度抖动

---

### 3. **改进 Feedback 算法** (中优先级)

**方案 A**: 增加增益
```c
#define UAC2_FB_GAIN 64  // 从 16 增加到 64
```

**方案 B**: 使用 I2S TX slab 作为反馈源 (已实现，但需验证)
```c
// uac2_pcm5102_app.c 中已正确使用 pcm5102a_fill_permille()
// 需要验证 slab 填充率是否与音频质量相关
```

**方案 C**: PI 控制器
```c
static int32_t g_fb_integral;

g_fb_integral += error;
g_fb_adjust = (error * UAC2_FB_P_GAIN) + (g_fb_integral * UAC2_FB_I_GAIN);
```

---

### 4. **DSP 处理超时保护** (低优先级)

```c
int64_t start_time = k_uptime_get();
play_dsp_process_planar(state, planarFrames, frameCount, channels);
int64_t elapsed = k_uptime_get() - start_time;

if (elapsed > 2) {  // 超过 2ms 警告
    LOG_WRN("DSP processing took %dms", (int)elapsed);
}
```

---

### 5. **I2S 驱动优化** (中优先级)

**方案 A**: 使用 DMA 双缓冲
```c
// 配置 I2S 使用双缓冲模式
.config.options = I2S_OPT_LOOPBACK | I2S_OPT_DMA_DOUBLE_BUFFER,
```

**方案 B**: 提高 I2S 优先级
```c
// 在 prj.conf 中
CONFIG_I2S_STM32_TX_PRIORITY=5
```

---

## 监控指标

### 关键日志字段 (来自 `uac2_pcm5102_app.c`)

```
UAC2 RX packets=%u bytes=%u frames=%u out_delivered=%u buffers=%u rejected=%u zero=%u invalid=%u odd=%u checksum=%u pipe_ok=%u pipe_fail=%u audio_drop=%u fb_adj=%d fb_calls=%u fb_fill_min=%d fb_fill_max=%d slab_min=%u slab_max=%u
```

**需要关注的指标**:
- `audio_drop > 0`: 表示 async queue 溢出
- `slab_max < 5`: 表示 slab 内存池太深，可优化
- `fb_adj` 波动大: 表示 feedback 调整剧烈，时钟不稳

---

## 测试建议

### 1. **压力测试**
```bash
# 持续播放高采样率音频
arecord -D hw:0,0 -f cd -d 60 /dev/null
```

### 2. **监控脚本**
```bash
# 监控 slab 使用情况
while true; do
    echo "Slab used: $(cat /sys/kernel/debug/pcm5102a_slab_used)"
    sleep 1
done
```

### 3. **逻辑分析仪**
- 监控 BCK、LRCK、DIN 信号
- 检查是否出现 bit clock glitches

---

## 总结

**最可能导致卡顿的原因**:
1. **I2S TX Slab allocation with K_MSEC(2) blocking in USB callback** (高风险)
2. **Async queue depth too shallow (only 3 blocks)** (中风险)
3. **Feedback gain too low (16), slow response** (中风险)

**推荐优先修复顺序**:
1. 将 `K_MSEC(2)` 改为 `K_NO_WAIT` + 备用 buffer
2. 增加 `UAC2_AUDIO_QUEUE_DEPTH` 从 3 到 8
3. 增加 `UAC2_FB_GAIN` 从 16 到 64
