# 低频 EQ（eq_low_seq）分析、实现与应用说明

## 1. 背景与目标

在当前工程中，已有线性相位 FIR EQ（`play_eq_linear`）和常规/动态 EQ 链路。

针对你提出的目标：

- 500Hz 以下，尤其 150Hz 以下重低音增强要“有效果”
- 要支持窄带增强（可聚焦到某个低频中心点）
- 相位偏移尽可能小
- 延时尽可能小（实时链路中尽量接近 0ms）

本次新增 `eq_low_seq` 模块，采用低频 peaking biquad 串联（IIR 最小相位），重点解决“低频增强有效性 + 低延时”问题。

## 2. 关键分析结论

### 2.1 为什么新增 IIR 低频段

在线性相位 FIR 方案里，若要求非常低延时（短 taps）且低频窄带增强明显，会受到频率分辨率和过渡带约束。

低频窄带提升本质上需要更高频域分辨率，这通常会推高 FIR 长度，从而带来更高群延时。

所以在“低频有效增强”和“低延时”之间，IIR peaking 是更实用的工程折中。

### 2.2 方案折中逻辑

- 低延时优先：采用 IIR，算法延时可视作 0 样本
- 增益有效优先：允许适当 Q 值，保证窄带提升足够明显
- 相位控制：提供频率相关 Q 上限，模式越“相位优先”越保守

## 3. 模块实现

### 3.1 文件与接口

- 头文件：`src/eq_low_seq.h`
- 实现：`src/eq_low_seq.c`

核心接口：

- `eq_low_seq_init`
- `eq_low_seq_set_mode`
- `eq_low_seq_set_band`
- `eq_low_seq_set_profile`
- `eq_low_seq_process_sample`
- `eq_low_seq_process_planar`
- `eq_low_seq_get_latency`
- `eq_low_seq_q_cap_for_freq`

### 3.2 三种模式

- `EQ_LOW_SEQ_MODE_GAIN_FOCUSED`
  - 允许更高 Q，上升更聚焦，增益视觉/听感最明显
- `EQ_LOW_SEQ_MODE_BALANCED`
  - 默认平衡模式
- `EQ_LOW_SEQ_MODE_PHASE_FOCUSED`
  - 更严格限制低频 Q，尽量减小相位扰动

### 3.3 频率相关 Q 上限

`eq_low_seq_q_cap_for_freq` 对低频段设置更保守的 Q 上限。频率越低，Q cap 越低，以减少过窄峰值导致的相位急剧旋转。

### 3.4 延迟特性

`eq_low_seq_get_latency` 返回 0 样本、0ms 算法延迟（IIR 路径）。

说明：系统端到端总延迟仍由音频设备 buffer、线程调度、其余处理链路共同决定；这里说的是模块算法附加延迟。

## 4. 应用到实际播放链路（play_macos）

### 4.1 接入位置

`play_macos` 的实时音频回调仍调用统一入口 `play_dsp_process_planar`。

本次在 `play_dsp_process_planar` 内新增 low-seq 处理步骤：

1. 输入平面缓冲进入 DSP
2. 先执行 `eq_low_seq_process_planar`（当 low-seq 启用且非全局 bypass）
3. 再进入原有 pre/eq/post pipeline

这样 low-seq 已经在真实播放路径上生效，不是离线验证专用。

### 4.2 默认配置

在初始化中给出 5 段低频 profile（可按听感再调）：

- 35Hz / +6.0dB / Q 2.2
- 60Hz / +6.0dB / Q 2.1
- 110Hz / +4.0dB / Q 1.8
- 220Hz / +2.5dB / Q 1.4
- 300Hz / +1.5dB / Q 1.2

默认模式：`BALANCED`。

## 5. 测试迁移到 tests/

按你的要求，测试代码已放到 `tests` 目录：

- `tests/test_eq_low_seq.c`

并保留构建目标名 `verify_eq_low_seq`，便于和已有验证工具命名风格保持一致。

## 6. 验证覆盖点

`tests/test_eq_low_seq.c` 当前覆盖：

1. 150Hz 以下增益有效性
   - 35 / 60 / 110Hz，检查输出增益是否达到有效阈值
2. 窄带增强形状
   - 比较中心频点与邻近/远端频点增益差
3. 算法延时
   - 校验 low-seq 报告为 0 样本 / 0ms
4. 相位优先模式约束
   - 检查 band Q 是否被 mode cap 限制

## 7. 构建与运行

### 7.1 构建验证程序

```bash
cmake -S . -B build
cmake --build build --target verify_eq_low_seq -j
```

### 7.2 运行验证

```bash
./verify_eq_low_seq
```

### 7.3 构建并运行播放链路

```bash
cmake --build build --target play_macos -j
./play_macos
```

## 8. 后续建议

如果你希望在 Web 控制页实时调 low-seq（启用、模式、每段 freq/gain/Q），下一步可以把 low-seq 参数扩展到：

- `GET /eq` 返回 low-seq 状态
- `POST /eq` 支持 low-seq 更新
- UI 增加 low-seq 控制面板

这样可以边听边调，快速找出你偏好的重低音窄带参数。