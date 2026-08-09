# play_dsp_state 结构体详解

本文档详细解释 src/play_dsp_common.h 中 play_dsp_state 的字段设计、运行时职责、生命周期，以及它和实时音频处理流程的对应关系。

## 1. 设计目标

play_dsp_state 是整个 DSP 链路的单一运行时状态容器，统一承载：

- 参数配置（EQ、动态参数、插件开关）
- 滤波器内部状态（IIR 历史项、SVF 积分器）
- 分析缓存（pre/post 频谱、相位差、群延迟）
- 可选加速缓冲（CMSIS-DSP FFT）

这样可以在音频回调线程中以最少开销访问状态，同时让 Web 控制接口只需要读写同一个对象。

## 2. 宏与容量约束（结构体字段维度来源）

这些宏定义决定了结构体数组大小和处理上限：

- ANALYZER_WINDOW = 512
- ANALYZER_BINS = 48
- MAX_CHANNELS = 2
- EQ_BANDS = 8

含义：

- 频谱分析每次基于 512 样本窗。
- 输出 48 个对数分布频点。
- 当前实现按最多 2 声道维护每段状态。
- EQ 固定 8 段。

## 3. 字段分组与职责

### 3.1 基本音频上下文

- uint32_t sampleRate
- uint32_t channels

作用：

- 决定滤波系数计算和频点映射。
- 约束每帧处理时的声道循环边界。

特点：

- 初始化后通常不频繁变动。

### 3.2 EQ 参数与模式选择

- float bandFreqs[EQ_BANDS]
- float gainsDB[EQ_BANDS]
- float qValues[EQ_BANDS]
- uint8_t bandModes[EQ_BANDS]
- uint8_t bandUseSVF[EQ_BANDS]

作用：

- 前三项是每段中心频率、增益、Q。
- bandModes 表示每段模式：Linear / Dynamic / Normal。
- bandUseSVF 表示该段是否走低频 SVF 路径（当前阈值由 EQ_LOW_SVF_MAX_HZ 控制）。

### 3.3 动态 EQ 参数与实时包络状态

- float dynamicEnv[EQ_BANDS][MAX_CHANNELS]
- float dynamicReductionDB[EQ_BANDS]
- float dynamicAttack
- float dynamicRelease
- float dynamicThreshold
- float dynamicMaxReductionDB
- float dynamicStrengthDB

作用：

- dynamicEnv 是每段每声道的包络跟踪状态。
- dynamicReductionDB 是当前每段衰减量（供可视化使用）。
- 其余参数控制动态压制行为。

运行机制简述：

- 动态模式段中，先更新包络，再根据阈值和强度计算衰减。
- 每段衰减量做平滑回写到 dynamicReductionDB。

### 3.4 功能开关与插件状态

- uint32_t pluginMask
- uint8_t bypassEnabled
- uint8_t lowCrossfeedEnabled
- uint8_t lowCrossfeedPosition

作用：

- pluginMask：位掩码开关，可扩展多个插件。
- bypassEnabled：总旁路开关，打开时跳过所有 EQ 和 mixed 效果。
- lowCrossfeedEnabled / lowCrossfeedPosition：低频串音混合开关与前后位置。

关键语义：

- bypass 只旁路处理路径，不清除已保存参数。
- 关闭 bypass 后立即恢复当前参数对应的处理效果。

### 3.5 低频 crossfeed 低通状态

- float lowCrossfeedLpA
- float lowCrossfeedLpState[MAX_CHANNELS]

作用：

- lowCrossfeedLpA 是一阶低通系数。
- lowCrossfeedLpState 是每声道滤波历史状态。

说明：

- 这是 IIR 必需的跨样本记忆项，不能每帧重置。

### 3.6 相位分析结果缓存

- float phaseDeltaDeg[ANALYZER_BINS]
- float groupDelayMs[ANALYZER_BINS]

作用：

- 保存每个频点的相位差和群延迟结果。
- Web 接口可以直接读取，不用前端重复计算。

### 3.7 Biquad 系数缓存

- double b0[EQ_BANDS]
- double b1[EQ_BANDS]
- double b2[EQ_BANDS]
- double a1[EQ_BANDS]
- double a2[EQ_BANDS]

作用：

- 存放每段 peaking EQ 的离散系数。
- 参数变化时重建，实时路径只做乘加，避免回调中高开销三角函数。

### 3.8 TPT-SVF 参数与积分器状态

- double svfG[EQ_BANDS]
- double svfK[EQ_BANDS]
- double svfA[EQ_BANDS]
- double svfH[EQ_BANDS]
- double svfIc1eq[EQ_BANDS][MAX_CHANNELS]
- double svfIc2eq[EQ_BANDS][MAX_CHANNELS]

作用：

- 前四项是 SVF 段参数。
- 后两项是每段每声道积分器状态。

意义：

- 低频段使用 SVF 可获得更好的数值稳定性与控制手感。

### 3.9 Biquad 运行时历史项

- double x1[EQ_BANDS][MAX_CHANNELS]
- double x2[EQ_BANDS][MAX_CHANNELS]
- double y1[EQ_BANDS][MAX_CHANNELS]
- double y2[EQ_BANDS][MAX_CHANNELS]

作用：

- IIR 结构历史输入输出样本。
- 与系数共同定义当前滤波器状态。

### 3.10 频谱分析缓存

- float preSamples[ANALYZER_WINDOW]
- float postSamples[ANALYZER_WINDOW]
- int writeIndex
- float preBins[ANALYZER_BINS]
- float postBins[ANALYZER_BINS]
- int binIndex[ANALYZER_BINS]

作用：

- preSamples/postSamples：保存处理前后单声道观测样本窗。
- writeIndex：循环写入指针。
- preBins/postBins：频谱显示输出，含平滑。
- binIndex：对数频率到 FFT/Goertzel 索引映射。

### 3.11 CMSIS-DSP 可选加速区

在 USE_CMSIS_DSP 条件下包含：

- arm_rfft_fast_instance_f32 rfft
- int rfftReady
- float fftIn[ANALYZER_WINDOW]
- float fftOut[ANALYZER_WINDOW]
- float fftMag[ANALYZER_WINDOW / 2]
- float fftLog[ANALYZER_WINDOW / 2]

作用：

- 若初始化成功，频谱分析走 RFFT 加速路径。
- 若失败，逻辑可回退到非 CMSIS 的 Goertzel 路径。

## 4. 生命周期与调用路径

### 4.1 初始化阶段

由 play_dsp_init 完成：

- 清零整个结构体。
- 设置默认 EQ 频点、默认增益/Q、动态参数、crossfeed 默认值。
- 生成频谱 bin 映射。
- 重建 EQ/SVF 系数。
- 初始化 CMSIS RFFT（若启用）。

### 4.2 参数更新阶段

由控制接口触发：

- play_dsp_set_eq_params 更新增益/Q，并触发模式分配与系数重建。
- play_dsp_set_dynamic_params 更新动态参数。
- play_dsp_set_low_crossfeed 更新低频混合开关和位置。
- play_dsp_set_bypass 更新旁路状态。
- play_dsp_set_plugin_enabled 更新插件掩码。

### 4.3 实时处理阶段

由 play_dsp_process 在音频回调中执行：

- 对每一帧、每一声道进行处理。
- 根据 bypassEnabled 决定是否跳过 EQ 和 mixed 效果。
- 在未 bypass 时按配置执行 pre/post crossfeed 与多段 EQ。
- 更新 dynamicEnv、dynamicReductionDB。
- 采集 pre/post 单声道样本到分析窗。
- 写满 ANALYZER_WINDOW 后更新频谱与相位指标。

### 4.4 数据导出阶段

- play_dsp_copy_spectrum 导出 pre/post 频谱。
- play_dsp_copy_dynamic_curve 导出动态段模式与衰减曲线。
- play_dsp_copy_phase_metrics 导出相位差与群延迟。
- play_dsp_copy_eq 导出 EQ 频点、增益、Q。

## 5. 线程与一致性建议

当前使用方式是音频线程与控制线程共享同一个 play_dsp_state，并通过外层互斥锁保护访问。

建议保持以下原则：

- 所有 setter/getter 与 process 调用都在同一把互斥锁保护下执行。
- 不要在未加锁情况下直接修改结构体字段。
- 避免在音频回调里做复杂内存分配或字符串操作。

## 6. 为什么这个结构体设计有效

- 单一真值源：参数、状态、分析结果集中管理。
- 实时友好：重计算集中在参数更新阶段，回调路径以乘加和状态推进为主。
- 可扩展：pluginMask 与条件编译使插件和平台加速可增量扩展。
- 可观测：pre/post 与相位指标都内建缓存，便于 Web 可视化和调试。

## 7. 当前版本重点（与最近改动相关）

- 已支持 bypassEnabled 全局旁路。
- bypass 打开时播放原始音频；关闭后恢复原参数对应效果。
- lowCrossfeed 与 EQ 参数在 bypass 期间仍被保留，不会丢失。
- pre/post 频谱仍可持续反映当前处理路径状态，便于观测开关影响。
