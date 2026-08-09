# play_dsp_state 结构与模块拆分说明

本文档描述当前代码中的真实结构与职责边界，基于以下实现文件：

- src/play_dsp_common.h
- src/play_dsp_common.c
- src/play_pipeline.h / src/play_pipeline.c
- src/play_pipeline_eq_stages.h / src/play_pipeline_eq_stages.c
- src/play_eq_linear.* / src/play_eq_dynamic.* / src/play_eq_normal.*
- src/play_multiband_dynamics.*

## 1. 结构体定位

play_dsp_state 是统一 DSP 运行态容器，负责保存：

- 参数层：EQ、Dynamic EQ、Linear FIR、Crossfeed、Multiband Dynamics、插件开关
- 算法历史态：FIR 历史、Biquad/SVF 状态、MB 分频 LP 状态
- 观测层：pre/post 频谱、EQ taps、phase/group-delay

定义位置：src/play_dsp_common.h

## 2. 关键宏与边界（当前实现）

- ANALYZER_WINDOW = 512
- ANALYZER_BINS = 48
- MAX_CHANNELS = 2
- EQ_BANDS = 16
- PLAY_MB_DYN_BANDS = 8

EQ 模式边界：

- EQ_LINEAR_MAX_HZ = 220.0f
- EQ_DYNAMIC_MAX_HZ = 16000.0f
- Normal 区：f > 16000Hz

默认 EQ 频点（16 段）：

- 20, 35, 60, 110, 220, 360, 700, 1600, 3200, 4800, 7200, 10000, 16000, 18000, 20000, 22000

默认增益：

- 低频前四段默认 +3dB
- 18000Hz 默认 -3dB
- 20000Hz 与 22000Hz 默认 -24dB

## 3. 字段分组

### 3.1 基础上下文

- sampleRate
- channels

### 3.2 EQ 参数与模式

- bandFreqs[EQ_BANDS]
- gainsDB[EQ_BANDS]
- qValues[EQ_BANDS]
- bandModes[EQ_BANDS]
- bandUseSVF[EQ_BANDS]

### 3.3 Dynamic EQ 运行态

- dynamicEnv[EQ_BANDS][MAX_CHANNELS]
- dynamicReductionDB[EQ_BANDS]
- dynamicAttack / dynamicRelease / dynamicThreshold
- dynamicMaxReductionDB / dynamicStrengthDB

### 3.4 Linear FIR 运行态

- linearFirEnabled
- linearFirUserEnabled
- linearFirTaps
- linearFirCoeff[] / linearFirHist[][]
- linearFirWriteIndex[] / linearFirTransientEnv[] / linearFirLastDelayed[]

### 3.5 Crossfeed 状态

- lowCrossfeedEnabled
- lowCrossfeedPosition
- lowCrossfeedLpA
- lowCrossfeedLpState[]

### 3.6 Multiband Dynamics 状态

- mbDynEnabled / mbDynPosition
- mbDynThreshold / mbDynAttack / mbDynRelease / mbDynStrength
- mbDynBandLowHz[] / mbDynBandHighHz[]
- mbDynBandAmountDb[] / mbDynBandAppliedDb[]
- mbDynEnv[][] / mbDynLpA[] / mbDynLpState[][]

### 3.7 分析与埋点

- preSamples[] / postSamples[]
- eqTapInSamples[] / eqTapOutSamples[]
- eqTapSeq / writeIndex
- preBins[] / postBins[] / binIndex[]
- phaseDeltaDeg[] / groupDelayMs[]

## 4. 模块职责边界

### 4.1 src/play_eq_linear.*

- 线性 FIR 配置与重建
- 样本处理函数与独立 ctx API

### 4.2 src/play_eq_dynamic.*

- 动态包络更新
- 动态衰减计算
- 参数结构体（可独立初始化/设置/读取）

### 4.3 src/play_eq_normal.*

- peaking / lowpass 系数构建函数
- 默认频点配置
- 模式分配与数组重建接口
- SVF 单样本处理函数

### 4.4 src/play_multiband_dynamics.*

- MB 参数初始化与钳制
- amount/applied/bands 拷贝接口
- 单样本 MB 处理核心

### 4.5 src/play_pipeline.*

- pipeline 容器与 stage 调度
- stage add/enable/bypass/run

### 4.6 src/play_pipeline_eq_stages.*

- linear/dynamic/normal stage
- pre/post crossfeed stage
- pre/post mbdyn stage
- observer stage（input/eq-in/eq-out/output）

## 5. 当前主链路顺序（play_dsp_process）

每帧执行顺序：

1. observe-input
2. pre-crossfeed
3. pre-mbdyn
4. observe-eq-in
5. linear-eq
6. dynamic-eq
7. normal-eq
8. observe-eq-out
9. post-mbdyn
10. post-crossfeed
11. observe-output

说明：

- bypass=ON 时，处理类 stages 被禁用，observer stages 仍保留。
- observer-output 负责写入 pre/eqTap/post 缓冲、推进 eqTapSeq/writeIndex，并在窗口满时触发频谱更新。

## 6. 一致性结论

当前实现已完成：

- 三类 EQ 与 MB 模块化拆分
- pre/post crossfeed 与 pre/post MB stage 化
- analyzer/tap observer stage 化
- 外部 API 兼容保持

因此，play_dsp_state 目前应视为“兼容外壳 + 统一运行态”，内部调度已是 pipeline 模型。
