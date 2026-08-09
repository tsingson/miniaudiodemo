# Audio EQ Guide

## 1. 当前能力概览

本项目当前是一个可实时调参的音频处理链，核心特性如下：

- 13 段 EQ（包含新增 10k / 12k / 15k / 16k / 17k 频点）
- 三种 EQ 模式自动分配（Linear / Dynamic / Normal）
- 18kHz 节点改为固定高切（12 dB/oct，二阶低通）
- 全局 bypass（打开后旁路 EQ 与 mixed 效果，但保留参数）
- 低频 crossfeed（可开关，可选 Pre EQ / Post EQ）
- 8 频段多段动态处理（multiband dynamics，范围 +/-6 dB）
- Web 可视化同时显示 MB Target 与 MB Applied 曲线
- 频谱图支持固定关键频点显示，并增加参考哈曼曲线

主入口：

- host 端：`play_macos`
- MCU 端：`play_mcu`
- 共用 DSP：`src/play_dsp_common.c`

## 2. 三类 EQ 规则

频段模式由频率自动决定：

- Linear: `f <= 2kHz`
- Dynamic: `2kHz < f <= 17kHz`
- Normal: `f > 17kHz`

对应宏定义见 `src/play_dsp_common.h`：

- `EQ_LINEAR_MAX_HZ = 2000.0f`
- `EQ_DYNAMIC_MAX_HZ = 17000.0f`

### 2.1 Linear EQ

- 线性模式不再做“压缩映射”，滑杆 dB 值直接作为目标增益。
- 低频段（`<= 1kHz`）优先走 TPT-SVF bell 结构，提高低频稳定性与手感。

### 2.2 Dynamic EQ

- 先做包络跟踪：
  - `env += coeff * (abs(out) - env)`
  - 上升用 `attack`，下降用 `release`
- 当该段用户增益为正且超过阈值时，按强度计算动态衰减。
- 衰减量受 `dynamicMaxReductionDB` 限幅。

### 2.3 Normal EQ

- 普通静态 peaking EQ。
- 18kHz 节点特殊处理为固定高切：
  - 类型：二阶低通
  - 坡度：12 dB/oct
  - 目的：超过 18kHz 的能量自然衰减

## 3. 13 段默认频点

初始化频点（`play_dsp_init`）当前为：

- 31, 62, 125, 250, 500, 2000, 8000, 10000, 12000, 15000, 16000, 17000, 18000 (Hz)

其中：

- 10k / 12k / 15k / 16k / 17k 为新增可调节点
- 18k 为高切节点（非普通 peaking）

## 4. 多段动态处理（Multiband Dynamics）

### 4.1 基本行为

- 固定 8 个频带分割，分别维护包络和实时作用量。
- 每个 band 有一个 `amountDb`（范围 +/-6 dB）。
- 当包络超过阈值后：
  - `amountDb > 0` 倾向向下压（避免过激）
  - `amountDb < 0` 倾向向上拉（做扩展倾向）

### 4.2 关键范围（来自头文件）

- `PLAY_MB_DYN_MIN_DB = -6.0f`
- `PLAY_MB_DYN_MAX_DB = 6.0f`
- `PLAY_MB_DYN_MIN_THRESHOLD = 0.01f`
- `PLAY_MB_DYN_MAX_THRESHOLD = 1.00f`
- `PLAY_MB_DYN_MIN_ATTACK = 0.002f`
- `PLAY_MB_DYN_MAX_ATTACK = 0.20f`
- `PLAY_MB_DYN_MIN_RELEASE = 0.005f`
- `PLAY_MB_DYN_MAX_RELEASE = 0.50f`
- `PLAY_MB_DYN_MIN_STRENGTH = 0.1f`
- `PLAY_MB_DYN_MAX_STRENGTH = 8.0f`

### 4.3 链路位置

多段动态支持两种插入位置：

- Pre EQ
- Post EQ

该位置由 `mbDynPosition` 控制，并在 `/eq` 接口中可读写。

## 5. 处理链路顺序

对每个采样点（每个声道）的整体顺序：

1. 读取输入样本
2. 若 bypass=OFF 且 mbDyn 开启且位置为 Pre EQ：执行 MB dyn
3. 若 bypass=OFF 且 lowCrossfeed 开启且位置为 Pre EQ：执行低频 crossfeed
4. 若 bypass=OFF：执行 13 段 EQ（含 dynamic 后处理）
5. 若 bypass=OFF 且 lowCrossfeed 开启且位置为 Post EQ：执行低频 crossfeed
6. 若 bypass=OFF 且 mbDyn 开启且位置为 Post EQ：执行 MB dyn
7. 写回输出并更新分析窗口

## 6. Web 端显示与接口

### 6.1 频谱与曲线

- 频谱显示精度已提高，按关键频点绘制（如 20/30/.../20k）。
- 频谱下方标签已去掉 "hz" 后缀，仅显示数值。
- 增加固定参考哈曼曲线（只作为视觉参考，不参与 DSP）。
- 新增 MB Target 与 MB Applied 两条曲线：
  - MB Target：配置目标（用户设定）
  - MB Applied：实时生效（受包络与阈值影响）

### 6.2 `/eq` GET

返回：

- EQ 基础参数与限制
- bypass / crossfeed / dynamic 参数
- multiband dynamics 开关、位置、全局参数
- multiband band 边界、target amounts、applied dB
- dynamicReductionDB 与 bandModes

### 6.3 `/eq` POST

支持部分字段增量更新，包括：

- gains / qs
- bypassEnabled
- lowCrossfeedEnabled / lowCrossfeedPosition
- dynamic 参数组
- mbDynEnabled / mbDynPosition / mbDynThreshold / mbDynAttack / mbDynRelease / mbDynStrength
- mbDynAmounts

## 7. 模块拆分（本次重构）

为降低 `play_dsp_common.c` 的耦合，已完成模块拆分：

- `src/play_eq_linear.h/.c`
  - 线性模式增益映射接口（当前为 dB 直通）
- `src/play_eq_dynamic.h/.c`
  - 动态 EQ 包络更新与衰减计算
- `src/play_eq_normal.h/.c`
  - peaking 与 12 dB/oct lowpass 系数构建
- `src/play_multiband_dynamics.h/.c`
  - 8 段多段动态处理与配置/拷贝接口

`src/play_dsp_common.c` 现在主要负责：

- 链路编排
- 状态管理
- 对外 API
- 分析数据更新

## 8. CMake 构建变化

`CMakeLists.txt` 已将新增模块加入两个目标：

- `play_macos`
- `play_mcu`

新增编译单元：

- `src/play_eq_linear.c`
- `src/play_eq_dynamic.c`
- `src/play_eq_normal.c`
- `src/play_multiband_dynamics.c`

## 9. 调参建议

1. 先用 Linear 区间（<=2k）定低频和主体密度。
2. 再用 Dynamic 区间（2k~17k）控制存在感和齿音风险。
3. 18k 高切按听感微调，避免过亮和尖锐超高频。
4. 最后启用 MB dyn，先小幅设置（如 +/-1~2dB），观察 Applied 曲线是否稳定跟随。
