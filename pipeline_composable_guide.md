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

建议定义统一帧块结构（可单样本，也可 N 帧批处理）：

- sampleRate
- channels
- frameCount
- interleavedSamples

建议命名：PlayFrameBlock。

### 3.2 阶段函数契约

建议每个阶段统一函数签名：

- 输入：阶段上下文、只读配置、帧块
- 输出：原地修改帧块或写入输出帧块
- 返回：状态码（0 成功，非 0 表示失败）

建议命名：PlayStageProcessFn。

### 3.3 阶段描述对象

每个阶段用一个描述对象承载：

- name：阶段名
- enabled：开关
- process：处理函数
- ctx：阶段运行态
- cfg：阶段配置

建议命名：PlayPipelineStage。

### 3.4 Pipeline 容器

Pipeline 容器维护有序阶段列表：

- stages[]
- stageCount
- bypass

执行规则：

- bypass 开启时可走短路路径
- 否则按序调用每个 enabled 阶段

建议命名：PlayPipeline。

## 4. 建议阶段划分

建议将现有主链路拆成以下阶段（顺序可配置）：

1. 输入观测阶段（采样前埋点）
2. pre-crossfeed
3. pre-multiband-dynamics
4. linear-eq
5. dynamic-eq
6. normal-eq
7. post-multiband-dynamics
8. post-crossfeed
9. 输出观测阶段（采样后埋点、频谱缓存）

说明：

- 线性/动态/模拟 EQ 建议拆为 3 个独立 stage，而不是混在一个 stage。
- 每个 stage 都应支持单独运行和单独测试。

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

## 9. 推荐落地步骤

1. 定义 Pipeline 公共头文件
- 新增 src/play_pipeline.h
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

## 12. 结论

本工程已经具备把主链路升级为可组合 pipeline 的关键前提。

下一阶段重点不是改算法，而是统一阶段契约和调度框架。

只要坚持 显式输入、显式状态、阶段隔离、分层测试 四个原则，就可以把主链路演进成：

- 可组合
- 可替换
- 可定位
- 可回归

并且不会破坏当前可运行版本。

## 13. 当前实现状态（2026-08-09）

当前代码已经完成 MVP 的第一轮落地：

- 已新增通用 pipeline 框架：src/play_pipeline.h、src/play_pipeline.c
- 已新增 EQ 三阶段 wrapper：src/play_pipeline_eq_stages.h、src/play_pipeline_eq_stages.c
- 已新增链路测试：tests/test_pipeline_chain.c
- 已把 test_pipeline_chain 接入 run_eq_unit_tests

主链路接入状态：

- play_dsp_process 内部的 EQ 内核（linear/dynamic/normal）已切换为 pipeline 调度
- crossfeed、multiband dynamics、analyzer/tap 仍保持原有外层编排
- 对外 API 保持兼容

当前验证结果：

- run_eq_unit_tests 通过（含 test_pipeline_chain）
- verify_eq_taps 通过

后续建议：

- 把 pre/post crossfeed 与 pre/post multiband dynamics 也封装为 stage，进入统一 pipeline
- 给 pipeline 增加阶段统计（延迟、NaN、裁剪计数）
- 新增双声道与阶段顺序扰动回归测试