# miniaudio 开发指南

## 项目结构

```
miniaudiodemo/
├── CMakeLists.txt
├── miniaudio_guide.md
├── src/
│   ├── main.c
│   ├── play_macos.c
│   ├── play_mcu.c
│   ├── play_dsp_common.c
│   ├── play_pipeline.c
│   ├── play_pipeline_eq_stages.c
│   ├── play_eq_linear.c
│   ├── play_eq_dynamic.c
│   ├── play_eq_normal.c
│   ├── play_multiband_dynamics.c
│   ├── verify_eq_gain.c
│   ├── verify_eq_taps.c
│   ├── miniaudio.c        ← 仅含 #define MINIAUDIO_IMPLEMENTATION + #include "miniaudio/miniaudio.h"
│   └── miniaudio.h        ← 单头文件库（不要修改）
└── tests/
    ├── test_eq_linear.c
    ├── test_eq_dynamic.c
    ├── test_eq_normal.c
    └── test_pipeline_chain.c
```

## 编译与运行

```bash
# 首次配置
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release

# 构建一个或多个目标
cmake --build build --target miniaudiodemo play_macos play_mcu -j 4

# 运行示例
./miniaudiodemo
./play_macos
```

> 注意：构建后可执行文件会自动复制到项目根目录，可直接运行，不需要切换到 build 目录。

---

## miniaudio 核心概念

### 1. 单头文件库的正确拆分

miniaudio 是单头文件（STB-style）库，`#define MINIAUDIO_IMPLEMENTATION` **只能出现一次**，否则链接报"重复定义"。

```
src/miniaudio.c   →  #define MINIAUDIO_IMPLEMENTATION + #include "miniaudio/miniaudio.h"
src/main.c        →  只 #include "miniaudio/miniaudio.h"（不加 define）
```

### 2. 音频回调（data_callback）

设备每隔固定时间（通常 10~20 ms）调用一次回调，要求填满 `frameCount` 帧数据。

```c
void data_callback(ma_device* pDevice, void* pOutput,
                   const void* pInput, ma_uint32 frameCount)
{
    ma_uint64 framesRead = 0;
    // 返回值是 ma_result（成功=0），帧数通过最后一个指针参数输出
    ma_decoder_read_pcm_frames(decoder, pOutput, frameCount, &framesRead);

    if (framesRead < frameCount) {
        // 到达文件末尾：循环播放
        ma_decoder_seek_to_pcm_frame(decoder, 0);
    }
}
```

**常见错误**：把 `ma_decoder_read_pcm_frames` 的返回值当帧数使用。该函数返回 `ma_result`（0 = MA_SUCCESS），实际读取的帧数通过第 4 个参数（`ma_uint64*`）输出。返回值误用会导致输出缓冲区永远为空，表现为静音。

### 3. ma_decoder — 文件解码器

```c
ma_decoder decoder;

// 打开文件（支持 WAV / MP3 / FLAC / OGG）
ma_decoder_init_file("test.wav", NULL, &decoder);

// 读取解码后的 PCM（已自动完成格式转换）
ma_decoder_read_pcm_frames(&decoder, buffer, frameCount, &framesRead);

// seek 到指定帧（用于循环）
ma_decoder_seek_to_pcm_frame(&decoder, 0);

// 释放
ma_decoder_uninit(&decoder);
```

解码器关键字段：

| 字段 | 类型 | 说明 |
|------|------|------|
| `outputFormat` | `ma_format` | 采样格式，如 `ma_format_f32` / `ma_format_s16` |
| `outputChannels` | `ma_uint32` | 声道数（单声道=1，立体声=2） |
| `outputSampleRate` | `ma_uint32` | 采样率（Hz），通常 44100 或 48000 |

### 4. ma_device — 播放设备

```c
ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
cfg.playback.format   = decoder.outputFormat;    // 与解码器匹配，避免额外转换
cfg.playback.channels = decoder.outputChannels;
cfg.sampleRate        = decoder.outputSampleRate;
cfg.dataCallback      = data_callback;
cfg.pUserData         = &myEngine;               // 传给回调的用户数据指针

ma_device device;
ma_device_init(NULL, &cfg, &device);  // NULL = 使用系统默认输出设备
ma_device_start(&device);

// 阻塞等待用户输入...

ma_device_uninit(&device);
```

### 5. macOS 平台链接依赖

```cmake
target_link_libraries(miniaudiodemo PRIVATE
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox"
)
```

Linux 需要链接 `pthread m dl`；Windows 无需额外依赖（使用 WinMM / WASAPI）。

---

## 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 静音，无报错 | 把 `ma_decoder_read_pcm_frames` 返回值当帧数 | 使用第 4 个参数获取帧数 |
| `无法打开 WAV 文件` | cwd 不是项目根目录 | 从项目根目录运行 `./miniaudiodemo` |
| 链接报重复符号 | `MINIAUDIO_IMPLEMENTATION` 定义了多次 | 只在 `miniaudio.c` 中定义一次 |
| 编译极慢 | `miniaudio.c` 含全部实现，约 8 万行 | 属正常现象，拆分后只编译一次 |
