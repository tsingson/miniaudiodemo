# 主链路可组合 Pipeline 设计说明

## 1. 目标

本文档定义一个可组合、可替换、可独立测试的音频主链路设计。

目标是让线性 EQ、动态 EQ、模拟 EQ 三类处理都满足以下要求：

- 可独立初始化
- 可独立调用
- 可独立单测
- 不依赖全局大状态
- 通过统一契约组合进主链路

设计上尽量接近函数式风格：

- 参数显式输入
- 状态显式传入/传出
- 阶段之间通过统一帧数据结构衔接
- 单阶段可做纯函数测试（确定输入得到确定输出）

## 2. 当前代码基础与可复用能力

目前工程已经具备三类 EQ 的独立接口基础：

- 线性 EQ：已有独立上下文与重建/处理接口
- 动态 EQ：已有独立参数结构与纯计算函数
- 模拟 EQ：已有数组化配置、模式分配、重建与 SVF 单样本纯处理

这意味着主链路可以进入下一层抽象：

- 由统一 Pipeline 编排阶段
- 每个阶段只关心自己的参数和状态
- 主链路只负责组合与调度

## 3. Pipeline 抽象

### 3.1 数据单元

当前实现使用统一帧块结构 `play_frame_block`（可单样本，也可 N 帧批处理）：

- sampleRate
- channels
- frameCount
- layout（interleaved / planar）
- interleaved（交错数据入口）
- planar（按声道平面数据入口）

当前推荐：主处理路径优先使用 planar（每声道独立平面），interleaved 作为 I/O 兼容壳。

### 3.2 阶段函数契约

当前实现阶段函数签名为 `int (*process)(void* ctx, play_frame_block* block)`：

- 输入：阶段上下文 + 帧块
- 输出：原地修改帧块
- 返回：状态码（0 成功，非 0 失败）

### 3.3 阶段描述对象

每个阶段由 `play_pipeline_stage` 描述：

- name：阶段名
- enabled：开关
- process：处理函数
- ctx：阶段运行态

### 3.4 Pipeline 容器

Pipeline 容器 `play_pipeline` 维护有序阶段列表：

- stages[]
- stageCount
- bypass

执行规则：

- bypass 开启时短路返回
- 否则按序调用每个 enabled 阶段

## 4. 建议阶段划分

当前主链路阶段顺序如下（固定顺序，按 stage enabled 执行）：

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

- 线性/动态/模拟 EQ 建议拆为 3 个独立 stage，而不是混在一个 stage。
- 每个 stage 都应支持单独运行和单独测试。

### 4.1 pre/post-mbdyn（多段动态压缩）说明

当前 `pre-mbdyn` 与 `post-mbdyn` 使用同一处理函数 `play_mbdyn_stage_process`，通过 `mbDynPosition` 决定在 pre 还是 post 生效。

处理要点（对应 `play_mb_dyn_process_sample`）：

- 固定 8 个频带：`[20,80] [80,150] [150,300] [300,700] [700,1500] [1500,4000] [4000,10000] [10000,16000]`
- 每个分频点使用一阶低通，得到 `low[i]`
- 通过差分重建 band：
  - `band0 = low0`
  - `bandi = lowi - low(i-1)`
  - `band7 = x - low6`
- 每个 band 独立包络跟踪：上升走 `attack`，下降走 `release`
- 当 `env > threshold` 且 `amountDb != 0` 时触发动态增减

Applied dB 规则：

- `over = env - threshold`
- `delta = clamp(over * 24 * strength, 0, abs(amountDb))`
- `amountDb >= 0` 时使用 `appliedDb = -delta`（压缩倾向）
- `amountDb < 0` 时使用 `appliedDb = +delta`（扩展倾向）

最后每个 band 乘上线性增益并求和回主信号，且 `mbDynBandAppliedDb[]` 做指数平滑，供观测层显示。

### 4.2 pre/post-crossfeed（低频 mixed 左右声道路由）说明

当前 `pre-crossfeed` 与 `post-crossfeed` 使用同一处理函数 `play_crossfeed_stage_process`，通过 `lowCrossfeedPosition` 决定在 pre 还是 post 生效。

仅在双声道 (`channels >= 2`) 且开关开启时处理。

路由过程：

1. 对左右声道分别做一阶低通，提取低频：
  - `lowL = LP(L)`
  - `lowR = LP(R)`
2. 低频互混写回：
  - `L' = L + ratio * lowR`
  - `R' = R + ratio * lowL`

当前常量：

- 低通截止：`PLAY_LOW_CROSSFEED_CUTOFF_HZ = 150Hz`
- 混合比例：`PLAY_LOW_CROSSFEED_RATIO = 0.40`

这实现的是“保留原通道主体 + 注入对侧低频”的低频 mixed 策略。

### 4.3 主链路信号流与关键公式（评审图）

```mermaid
flowchart LR
    In[Input Frame] --> OI[observe-input]
    OI --> PC[pre-crossfeed]
    PC --> PM[pre-mbdyn]
    PM --> OEI[observe-eq-in]
    OEI --> LEQ[linear-eq]
    LEQ --> DEQ[dynamic-eq]
    DEQ --> NEQ[normal-eq]
    NEQ --> OEO[observe-eq-out]
    OEO --> PoM[post-mbdyn]
    PoM --> PoC[post-crossfeed]
    PoC --> OO[observe-output]
    OO --> Out[Output Frame]

    subgraph Crossfeed_Low_Mixed
      C1[lowL = LP(L), lowR = LP(R)]
      C2[L' = L + ratio * lowR]
      C3[R' = R + ratio * lowL]
      C1 --> C2 --> C3
    end

    subgraph MB_Dynamics
      M1[Split 8 bands via LP + diff]
      M2[env attack/release tracking]
      M3[delta = clamp((env-thr)*24*strength, 0, |amount|)]
      M4[appliedDb = -delta if amount>=0 else +delta]
      M5[band *= 10^(appliedDb/20), then sum bands]
      M1 --> M2 --> M3 --> M4 --> M5
    end

    PC -. uses .-> Crossfeed_Low_Mixed
    PoC -. uses .-> Crossfeed_Low_Mixed
    PM -. uses .-> MB_Dynamics
    PoM -. uses .-> MB_Dynamics
```

图例说明：

- `pre-crossfeed/post-crossfeed` 使用同一低频 mixed 路由公式，差异仅在链路位置。
- `pre-mbdyn/post-mbdyn` 使用同一 8 段动态流程，差异仅在链路位置。
- `observe-output` 负责写入 `pre/eqTap/post` 缓冲并推进 `eqTapSeq/writeIndex`，窗口满时触发频谱更新。

## 5. 三类 EQ 的函数式边界

### 5.1 线性 EQ

建议边界：

- 配置输入：频点数组、增益数组、Q 数组、mode 数组
- 重建输出：FIR coeff
- 处理输入：ctx + 单样本
- 处理输出：单样本

可测试点：

- taps 规范化
- 重建是否启用
- 关闭时旁路一致性
- 固定输入序列的确定性输出

### 5.2 动态 EQ

建议边界：

- 配置输入：attack/release/threshold/maxReduction/strength
- 纯计算：env 更新、reduction 计算
- 状态输入输出：每 band 每通道 env

可测试点：

- 上升沿走 attack
- 下降沿走 release
- threshold 门限
- maxReduction 上限钳制

### 5.3 模拟 EQ

建议边界：

- 配置输入：freq/gain/Q/mode
- 重建输出：biquad 或 svf 参数数组
- 处理输入：系数 + 状态 + 样本
- 处理输出：样本 + 更新后的状态

可测试点：

- peaking 中心频点增益
- lowpass 通带/阻带
- mode 分配
- svf 单样本数值稳定性

## 6. 主链路组合策略

建议提供两种组合方式：

### 6.1 固定顺序组合

- 在初始化时固定阶段顺序
- 运行时只切 enabled
- 实现简单，适合当前工程

### 6.2 策略驱动组合

- 按配置选择阶段顺序
- 支持 pre/post 位置动态重排
- 更灵活，但实现更复杂

当前建议：先落地固定顺序，再增加策略驱动。

## 7. 错误隔离与可观测性

每个阶段建议输出统一统计信息：

- processedFrames
- clippedSamples
- nanCount
- stageLatencyUs

主链路聚合这些统计，可用于：

- 回归时定位哪一段异常
- Web 端展示阶段健康度
- 自动化测试中的性能阈值判定

## 8. 单测分层建议

建议保持 3 层测试：

### 8.1 函数层（纯函数）

- 直接测数学函数、钳制函数、系数构建函数

### 8.2 模块层（单 stage）

- 给定固定输入序列，验证单 stage 输出与状态推进

### 8.3 链路层（pipeline integration）

- 多 stage 组合后验证：
  - bypass 准确性
  - tap 时序完整性
  - 增益变化是否符合预期
  - interleaved 与 planar 行为一致性

### 8.4 Planar 专项回归

建议把以下用例固定到 CI：

- planar stage 顺序回归（乘后加 vs 加后乘）
- planar crossfeed pre/post 启停回归
- planar multiband dynamics pre/post 启停回归
- interleaved 与 planar 在同输入下的结果偏差阈值检查

## 9. 推荐落地步骤

1. 定义 Pipeline 公共头文件
- 新增 src/dspeq/play_pipeline.h
- 定义 FrameBlock、StageFn、Stage、Pipeline

2. 为三类 EQ 补齐 stage 包装
- linear stage 封装已有 ctx
- dynamic stage 封装 env/reduction 流程
- normal stage 封装 biquad/svf 流程

3. 在主链路里替换手写编排
- play_dsp_process 中改为调用 pipeline_run

4. 增加 pipeline 单测
- tests/test_pipeline_chain.c
- 覆盖 stage enable/disable、顺序一致性、bypass

5. 保持兼容 API
- 对外接口保持不变
- 内部逐步迁移到 pipeline

## 10. 与当前项目的一致性建议

结合当前工程现状，建议遵循：

- 不立即删除 play_dsp_state
- 先将 play_dsp_state 作为兼容外壳
- 内部阶段逐步转为独立 ctx
- 每迁移一个 stage 就补一组模块层测试
- 保持 stage 对两种布局兼容，但新增能力优先按 planar 设计

这样可以保证：

- 线上行为稳定
- 重构风险可控
- 文档、测试、实现同步演进

## 11. 最小可运行原型定义

建议先完成一个最小 pipeline 原型（MVP）：

- 阶段数：linear + dynamic + normal
- 输入：单声道单样本
- 输出：单样本
- 测试：
  - 三阶段都开启
  - 仅开启某一阶段
  - 全关闭（应等于输入）

MVP 通过后，再扩展到：

- 双声道
- block 处理
- pre/post crossfeed
- pre/post mb dynamics

以上扩展项当前均已落地。

## 12. 结论

本工程已经具备把主链路升级为可组合 pipeline 的关键前提。

下一阶段重点不是改算法，而是统一阶段契约和调度框架。

只要坚持 显式输入、显式状态、阶段隔离、分层测试 四个原则，就可以把主链路演进成：

- 可组合
- 可替换
- 可定位
- 可回归

并且不会破坏当前可运行版本。

## 13. 多声道 / 多音轨前瞻设计（5.1 / 7.1 / 复合音轨）

为支持 L/R 先上混到 5.1/7.1，再做每声道 EQ 与干湿混合，建议在当前模型上增加三层抽象。

### 13.1 Channel Plan（声道计划层）

目标：把“输入声道布局 -> 处理声道布局”独立成显式步骤。

- 输入可为 stereo、5.1、7.1 或自定义 bus
- 上混/下混矩阵单独模块化（不与 EQ stage 耦合）
- 输出统一为 planar 通道组，供后续 DSP stage 使用

建议接口方向：

- `play_channel_plan_prepare(...)`：准备矩阵与路由描述
- `play_channel_plan_apply(...)`：执行帧块级上混/下混

### 13.2 Per-Channel DSP Graph（每声道独立图）

目标：每个声道可独立挂 EQ/动态/其他效果。

- 每个声道拥有独立 stage chain 或共享拓扑+独立参数
- 声道状态（滤波历史、动态包络）严格按声道隔离
- 支持 L/R、C、LFE、Ls、Rs、Lb、Rb 独立参数

在当前代码上，planar 已提供良好基础：

- 每个 `planar[ch]` 天然对应一个独立声道平面
- stage 内可按 `ch` 单独选择参数组

### 13.3 Dry/Wet Mix Layer（干湿混合层）

目标：允许每声道、每 bus 的可变混合比例。

建议采用并行路径：

1. dryPath：保留原信号
2. wetPath：经过 EQ/动态等处理
3. mix：`out = dryGain * dry + wetGain * wet`

其中 `dryGain/wetGain` 可按：

- 全局统一
- 每声道独立
- 每 bus 独立

并建议保留等功率混合选项，避免感知响度突变。

### 13.4 建议的渐进落地顺序

1. 固化 planar 一致性测试（已开始）
2. 增加 Channel Plan 模块（先支持 stereo<->5.1）
3. 引入每声道参数表（EQ/动态）
4. 加入 dry/wet stage（先全局，再每声道）
5. 扩展到 7.1 与多 bus 复合音轨

## 14. 当前实现状态（2026-08-09）

当前代码已经完成可组合主链路的第一轮完整落地：

- 已新增通用 pipeline 框架：src/dspeq/play_pipeline.h、src/dspeq/play_pipeline.c
- 已新增 EQ 三阶段 wrapper：src/dspeq/play_pipeline_eq_stages.h、src/dspeq/play_pipeline_eq_stages.c
- 已新增 Channel Plan 模块骨架：src/play_channel_plan.h、src/play_channel_plan.c（stereo -> 5.1 planar 路由）
- 已新增链路测试：tests/test_pipeline_chain.c
- 已新增 Channel Plan 单测：tests/test_channel_plan.c
- 已把 test_pipeline_chain 接入 run_eq_unit_tests

主链路接入状态：

- play_dsp_process 内部已切换为 pre/eq/post 三段 pipeline 编排
- pre/post crossfeed 与 pre/post multiband dynamics 已封装为 stage 并接入统一 pipeline
- analyzer/tap 已封装为 observer stages（input/eq-in/eq-out/output）
- 对外 API 保持兼容

当前验证结果：

- run_eq_unit_tests 通过（含 test_pipeline_chain）
- verify_eq_taps 通过

后续建议：

- 给 pipeline 增加阶段统计（延迟、NaN、裁剪计数）
- 增加 observer 边界条件专项回归（窗口边界、跨帧一致性）
- 在 Channel Plan 上继续扩展 stereo <-> 7.1、可配置矩阵持久化与能量守恒回归