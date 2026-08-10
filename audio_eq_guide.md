# Audio EQ Guide

## 1. 当前能力概览

本项目当前是一个可实时调参的音频处理链，核心特性如下：

- 16 段 EQ
- 三种 EQ 模式自动分配（Linear / Dynamic / Normal）
- 全局 bypass（保留参数，关闭处理）
- 低频 crossfeed（开关 + Pre/Post 位置）
- 8 段 multiband dynamics（范围 +/-6 dB）
- observer stages 统一输出 pre/eqTap/post 与频谱窗口
- 可选 CMSIS-DSP 频谱计算路径（arm_rfft_fast_f32 等）

主入口：

- host 端：play_macos
- MCU 端：play_mcu
- 共用 DSP：src/play_dsp_common.c

## 2. 三类 EQ 规则

频段模式由频率自动决定：

- Linear: f <= 220Hz
- Dynamic: 220Hz < f <= 16000Hz
- Normal: f > 16000Hz

对应宏定义见 src/play_dsp_common.h：

- EQ_LINEAR_MAX_HZ = 220.0f
- EQ_DYNAMIC_MAX_HZ = 16000.0f

## 3. 默认 16 段频点

初始化频点当前为：

- 20, 35, 60, 110, 220, 360, 700, 1600, 3200, 4800, 7200, 10000, 16000, 18000, 20000, 22000 (Hz)

默认增益：

- 20/35/60/110 为 +3dB
- 18000 为 -3dB
- 20000/22000 为 -24dB

## 4. 处理链路顺序

play_dsp_process 当前每帧顺序：

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

- 处理类 stage 在 bypass=ON 时会禁用。
- observer stages 保留，用于 tap 与频谱缓存连续更新。

## 5. Dynamic EQ 要点

- 包络更新：上升走 attack、下降走 release。
- 仅在该 band 用户增益为正且超过阈值时计算动态衰减。
- 衰减由 maxReductionDB 限幅。

## 6. Multiband Dynamics 要点

- 固定 8 个频带，默认分段：
  - [20,80], [80,150], [150,300], [300,700], [700,1500], [1500,4000], [4000,10000], [10000,16000]
- 每段 amount 范围：[-6, +6] dB
- 支持 Pre EQ / Post EQ 插入

算法流程（单样本、单声道）：

1. 用 7 个一阶低通分频，得到 8 个 band 分量。
2. 每个 band 独立更新包络 `env`：
  - 上升：`env += attack * (abs(x) - env)`
  - 下降：`env += release * (abs(x) - env)`
3. 当 `env > threshold` 且该 band `amountDb != 0`，计算动态量：
  - `over = env - threshold`
  - `delta = clamp(over * 24 * strength, 0, abs(amountDb))`
  - `amountDb >= 0` => `appliedDb = -delta`（压缩倾向）
  - `amountDb < 0` => `appliedDb = +delta`（扩展倾向）
4. `band *= 10^(appliedDb/20)` 后再把 8 个 band 求和回输出。

观测指标：

- `mbDynBandAmountDb[]`：目标值（UI 配置）
- `mbDynBandAppliedDb[]`：实时生效值（平滑后）

关键范围（src/play_dsp_common.h）：

- PLAY_MB_DYN_MIN_DB = -6.0f
- PLAY_MB_DYN_MAX_DB = 6.0f
- PLAY_MB_DYN_MIN_THRESHOLD = 0.01f
- PLAY_MB_DYN_MAX_THRESHOLD = 1.00f
- PLAY_MB_DYN_MIN_ATTACK = 0.002f
- PLAY_MB_DYN_MAX_ATTACK = 0.20f
- PLAY_MB_DYN_MIN_RELEASE = 0.005f
- PLAY_MB_DYN_MAX_RELEASE = 0.50f
- PLAY_MB_DYN_MIN_STRENGTH = 0.1f
- PLAY_MB_DYN_MAX_STRENGTH = 8.0f

## 7. 观测与可视化

当前观测数据由 observer stages 写入：

- preSamples / postSamples
- eqTapInSamples / eqTapOutSamples
- eqTapSeq / writeIndex

窗口满时触发频谱更新，并可选更新 phase/group-delay（由 pluginMask 控制）。

## 8. 低频 mixed（左右声道路由）

低频 mixed 由 crossfeed stage 完成，仅在立体声下生效。

步骤：

1. 左右声道分别提取低频：
  - `lowL = LP(L)`
  - `lowR = LP(R)`
2. 交叉注入低频：
  - `L' = L + ratio * lowR`
  - `R' = R + ratio * lowL`

当前参数：

- 截止频率：`PLAY_LOW_CROSSFEED_CUTOFF_HZ = 150Hz`
- 混合比例：`PLAY_LOW_CROSSFEED_RATIO = 0.40`

可放置位置：

- Pre EQ：先做低频互混，再进 EQ
- Post EQ：先做 EQ，再做低频互混

## 9. 模块拆分状态

已拆分模块：

- src/play_eq_linear.h/.c
- src/play_eq_dynamic.h/.c
- src/play_eq_normal.h/.c
- src/play_multiband_dynamics.h/.c
- src/play_pipeline.h/.c
- src/play_pipeline_eq_stages.h/.c

其中主链路编排集中在 src/play_dsp_common.c。

## 10. 构建与回归建议

推荐最小回归命令：

```bash
cmake -S . -B build
cmake --build build --target run_eq_unit_tests verify_eq_taps verify_eq_gain play_macos play_mcu -j 4
./verify_eq_taps
./verify_eq_gain
```

建议关注：

- test_pipeline_chain 是否通过（阶段顺序/启停回归）
- verify_eq_taps 是否通过（tap 时序与准确性）

## 11. 面向 5.1 / 7.1 与多音轨的设计考虑

当前实现已支持 planar 数据路径，这是后续多声道/复合音轨处理的基础。

建议演进方向：

1. 先做 Channel Plan
- 把 stereo -> 5.1 / 7.1 的上混逻辑独立出来。
- 在 EQ 前把信号整理为统一 planar 通道集。

2. 再做每声道独立 EQ/DSP
- 每个声道单独参数：L/R/C/LFE/Ls/Rs/Lb/Rb。
- 每声道独立状态，避免跨声道状态污染。

3. 增加 dry/wet 混合层
- 干路保真、湿路处理，再按比例混合：`out = dryGain * dry + wetGain * wet`。
- 比例可全局、可每声道、可每 bus。

4. 回归策略升级
- interleaved 与 planar 结果偏差门限回归。
- stereo 与 5.1/7.1 的能量守恒与通道串扰回归。

当前已落地的第一步：

- 新增 `play_channel_plan` 模块（stereo -> 5.1，planar 路由）
- 新增 `test_channel_plan` 回归并接入 `run_eq_unit_tests`
