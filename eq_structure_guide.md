# play_dsp_state 结构与模块拆分说明

本文档说明当前 `play_dsp_state` 的真实结构、字段职责，以及本次将三类 EQ 与多段动态处理拆分到独立模块后的架构关系。

## 1. 结构体定位

`play_dsp_state` 定义在 `src/play_dsp_common.h`，是整个音频处理链的统一运行时状态容器，负责保存：

- 参数：EQ、Dynamic EQ、Crossfeed、Bypass、Multiband Dynamics、插件开关
- 滤波器内部历史状态：Biquad / TPT-SVF / 多段动态分频 LP 状态
- 可视化缓存：pre/post 频谱、dynamic reduction、MB applied、相位差、群延迟

本次已将定义改为具名结构：

- `typedef struct play_dsp_state { ... } play_dsp_state;`

这样模块头文件可以用前向声明 `typedef struct play_dsp_state play_dsp_state;` 安全引用。

## 2. 容量与关键宏

当前核心宏（`src/play_dsp_common.h`）：

- `ANALYZER_WINDOW = 512`
- `ANALYZER_BINS = 48`
- `MAX_CHANNELS = 2`
- `EQ_BANDS = 13`
- `PLAY_MB_DYN_BANDS = 8`

EQ 模式边界：

- `EQ_LINEAR_MAX_HZ = 2000.0f`
- `EQ_DYNAMIC_MAX_HZ = 17000.0f`
- `>17000Hz` 进入 Normal 区
- `>=18000Hz` 当前实现使用固定高切系数构建

## 3. 字段分组详解

### 3.1 基础音频上下文

- `sampleRate`
- `channels`

用于系数重建、频谱映射、每帧循环边界控制。

### 3.2 EQ 参数与模式

- `bandFreqs[EQ_BANDS]`
- `gainsDB[EQ_BANDS]`
- `qValues[EQ_BANDS]`
- `bandModes[EQ_BANDS]`
- `bandUseSVF[EQ_BANDS]`

含义：

- 三类 EQ 自动按频率分配模式（Linear/Dynamic/Normal）。
- `bandUseSVF` 额外控制低频（<=1kHz）走 TPT-SVF 路径。

### 3.3 Dynamic EQ 运行态

- `dynamicEnv[EQ_BANDS][MAX_CHANNELS]`
- `dynamicReductionDB[EQ_BANDS]`
- `dynamicAttack`
- `dynamicRelease`
- `dynamicThreshold`
- `dynamicMaxReductionDB`
- `dynamicStrengthDB`

说明：

- `dynamicEnv` 是按段按声道的实时包络。
- `dynamicReductionDB` 用于 Web 显示每段实时衰减。

### 3.4 全局效果控制

- `pluginMask`
- `bypassEnabled`
- `lowCrossfeedEnabled`
- `lowCrossfeedPosition`

关键语义：

- `bypassEnabled=1` 时旁路 EQ 与 mixed 处理，但不丢失参数。
- 关闭 bypass 后恢复当前配置效果。

### 3.5 低频 crossfeed 状态

- `lowCrossfeedLpA`
- `lowCrossfeedLpState[MAX_CHANNELS]`

用于低频一阶低通与通道间混合，支持 Pre/Post EQ 位置切换。

### 3.6 Multiband Dynamics 状态

- `mbDynEnabled`
- `mbDynPosition`
- `mbDynThreshold`
- `mbDynAttack`
- `mbDynRelease`
- `mbDynStrength`
- `mbDynBandLowHz[PLAY_MB_DYN_BANDS]`
- `mbDynBandHighHz[PLAY_MB_DYN_BANDS]`
- `mbDynBandAmountDb[PLAY_MB_DYN_BANDS]`
- `mbDynBandAppliedDb[PLAY_MB_DYN_BANDS]`
- `mbDynEnv[PLAY_MB_DYN_BANDS][MAX_CHANNELS]`
- `mbDynLpA[PLAY_MB_DYN_BANDS - 1]`
- `mbDynLpState[PLAY_MB_DYN_BANDS - 1][MAX_CHANNELS]`

说明：

- Amount 是用户目标，Applied 是实时作用量（平滑后）。
- 每个分频点用一阶 LP 分离频带，再进行每带动态增减。

### 3.7 EQ 系数与滤波历史

Biquad：

- `b0/b1/b2/a1/a2`
- `x1/x2/y1/y2`

TPT-SVF：

- `svfG/svfK/svfA/svfH`
- `svfIc1eq/svfIc2eq`

这些字段都是实时滤波“记忆状态”，不能随意清零。

### 3.8 分析缓存

- `preSamples/postSamples`
- `writeIndex`
- `preBins/postBins`
- `binIndex`
- `phaseDeltaDeg/groupDelayMs`

作用：将处理前后可观测数据直接缓存在 DSP 状态中，便于 Web 拉取。

## 4. 模块拆分后的职责边界

### 4.1 src/play_eq_linear.h/.c

职责：Linear 模式映射。

- `play_eq_linear_map_gain_db(...)`

当前行为：直接返回滑杆 dB（不压缩）。

### 4.2 src/play_eq_dynamic.h/.c

职责：Dynamic EQ 的基础计算。

- `play_eq_dynamic_update_env(...)`
- `play_eq_dynamic_compute_reduction(...)`

`play_dsp_common.c` 在处理每个 dynamic band 时调用这两个函数。

### 4.3 src/play_eq_normal.h/.c

职责：Normal EQ / 高切系数构建。

- `play_eq_normal_build_peaking(...)`
- `play_eq_normal_build_lowpass_12db(...)`

`rebuild_eq(...)` 在 18k 节点调用 lowpass 构建函数。

### 4.4 src/play_multiband_dynamics.h/.c

职责：多段动态完整实现。

- 初始化：`play_mb_dyn_init(...)`
- 配置：`play_mb_dyn_set/get_config(...)`
- 参数复制：`play_mb_dyn_copy_*`
- 核心处理：`play_mb_dyn_process_sample(...)`

### 4.5 src/play_dsp_common.c

职责：链路 orchestration 与 API 汇总。

- 管理默认参数
- 分配 band 模式
- 重建 EQ（调用上述模块）
- 在 pre/post 位置插入 crossfeed 与 mb dyn
- 输出频谱、动态曲线、相位指标

## 5. 处理流程（单样本单声道）

1. 输入样本
2. 可选 MB dyn pre
3. 可选 crossfeed pre
4. 13 段 EQ（SVF/Biquad + dynamic control）
5. 可选 crossfeed post
6. 可选 MB dyn post
7. 写回并更新分析缓存

这保证了链路行为更清晰、可预测，也更容易在 Web 端解释“目标曲线 vs 实效曲线”。

## 6. 与文档/接口一致性

当前 API 已覆盖并暴露：

- 三类 EQ 参数
- dynamic 参数
- bypass/crossfeed
- multiband dynamics 配置与 band 数据
- dynamic reduction + mb applied 实时数据

因此前端可以同时呈现：

- 静态目标（EQ/MB Target）
- 动态实效（Dynamic Reduction/MB Applied）

