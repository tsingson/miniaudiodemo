# EQ Test Guide

## 1. 目标

本文档用于验证以下命题：

- 在低频区和中频区的每一个 EQ 频点上，设置 +3 dB / -3 dB 后，输入流在 EQ 前后的幅度变化应与设置值一致。

项目当前模式边界：

- 低频（Linear EQ）：f <= 220 Hz
- 中频（Dynamic EQ）：220 Hz < f <= 16 kHz
- 高频（Normal EQ）：f > 16 kHz

本测试仅覆盖低频 + 中频。

## 2. 测试代码位置

- 测试程序源文件：src/verify_eq_gain.c
- 构建目标：verify_eq_gain

## 3. 如何运行

```bash
cmake -S . -B build
cmake --build build --target verify_eq_gain -j 4
./verify_eq_gain
```

程序会逐 case 输出：

- band index
- 频点
- 目标增益（+3 / -3 dB）
- 实测增益
- 误差
- PASS / FAIL

## 4. 测试算法要点

### 4.1 激励信号

- 单频正弦波（每个 case 对应一个中心频点）
- 采样率：48 kHz
- 幅度：0.08
- 单声道测试（1 channel）

### 4.2 稳态测量策略

每个 case 分两段：

1. Warmup 段：8192 帧
- 用于让滤波器状态、FIR 缓冲、动态包络进入稳定区，减少初始瞬态干扰。

2. Measure 段：16384 帧
- 累积输入与输出平方和，计算 RMS。

### 4.3 增益计算

设：

- $R_{in}$ 为测量段输入 RMS
- $R_{out}$ 为测量段输出 RMS

则实测增益：

$$
G_{meas} = 20\log_{10}\left(\frac{R_{out}}{R_{in}}\right)
$$

目标增益：

- $G_{target}=+3\text{ dB}$ 或 $-3\text{ dB}$

误差定义：

$$
E = G_{meas} - G_{target}
$$

### 4.4 判定门限

- 容差：0.8 dB
- 判定条件：$|E|\le 0.8\text{ dB}$ -> PASS，否则 FAIL。

### 4.5 为避免压缩行为干扰

测试程序在每个 case 中会将动态相关效果置为“零影响”状态：

- Dynamic EQ：maxReduction 设为 0
- Multiband Dynamics：关闭并将 amount 清零

这样可验证“频点增益指令 -> 幅度变化”的主链路一致性。

## 5. 测试 case 设计说明

程序自动从 DSP 读取当前频点列表，然后在低频 + 中频频点上生成 case。

### 5.1 低频区 case（Linear EQ）

频点（当前默认）：

- 20, 35, 60, 110, 220 Hz

每个频点 2 个 case：

1. +3 dB
2. -3 dB

目的：

- 验证低频线性段在中心频点上的增益追踪是否准确。

### 5.2 中频区 case（Dynamic EQ）

频点（当前默认）：

- 360, 700, 1600, 3200, 4800, 7200, 10000, 16000 Hz

每个频点 2 个 case：

1. +3 dB
2. -3 dB

目的：

- 验证中频动态段在“动态抑制关闭”条件下，静态增益追踪是否准确。

### 5.3 case 总数

- 低频 5 点 + 中频 8 点 = 13 个频点
- 每点 2 个用例（+3 / -3）
- 合计 26 个用例

## 6. 输出字段解读

输出示例：

```text
band= 7 freq= 1600.0Hz target=  3.0dB measured=  3.000dB err=-0.000dB PASS
```

字段意义：

- band：当前频段索引
- freq：测试频点
- target：设置值
- measured：EQ 前后实测变化
- err：measured - target
- PASS/FAIL：是否满足容差

## 7. 已知现象与定位建议

如果出现“某些区段大量 FAIL、某些区段大量 PASS”，常见原因包括：

1. 该区段的算法不是纯静态 peaking，存在映射或混合策略
2. 频点模式分配与预期不一致（Linear/Dynamic/Normal 边界）
3. 动态或多段动态仍在产生非零影响
4. 中心频点定义与测试频率不一致
5. FIR/IIR 路径叠加导致目标 dB 被折减

建议排查顺序：

1. 先确认模式边界（EQ_LINEAR_MAX_HZ、EQ_DYNAMIC_MAX_HZ）
2. 确认动态压缩是否被完全抑制（尤其 maxReduction 与 MB amount）
3. 单独验证某一个失败频点，打印该 band 的实际 mode、Q、系数或 FIR 状态
4. 将容差固定后，对每个区段分别统计通过率

## 8. 测试代码（完整）

```c
#include "play_dsp_common.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_SAMPLE_RATE 48000u
#define TEST_CHANNELS 1u
#define WARMUP_FRAMES 8192
#define MEASURE_FRAMES 16384
#define TEST_TOLERANCE_DB 0.8

static float rms_for_band(play_dsp_state* dsp, float freqHz, float* outPostRms)
{
    float buf[TEST_CHANNELS];
    double inSum = 0.0;
    double outSum = 0.0;
    double phase = 0.0;
    double phaseStep = 2.0 * M_PI * (double)freqHz / (double)TEST_SAMPLE_RATE;
    const float amp = 0.08f;

    for (int i = 0; i < WARMUP_FRAMES; ++i)
    {
        float in = amp * (float)sin(phase);
        buf[0] = in;
        play_dsp_process(dsp, buf, 1, TEST_CHANNELS);
        phase += phaseStep;
        if (phase >= 2.0 * M_PI)
        {
            phase -= 2.0 * M_PI;
        }
    }

    for (int i = 0; i < MEASURE_FRAMES; ++i)
    {
        float in = amp * (float)sin(phase);
        float out;
        buf[0] = in;
        play_dsp_process(dsp, buf, 1, TEST_CHANNELS);
        out = buf[0];

        inSum += (double)in * (double)in;
        outSum += (double)out * (double)out;

        phase += phaseStep;
        if (phase >= 2.0 * M_PI)
        {
            phase -= 2.0 * M_PI;
        }
    }

    if (outPostRms != NULL)
    {
        *outPostRms = (float)sqrt(outSum / (double)MEASURE_FRAMES);
    }
    return (float)sqrt(inSum / (double)MEASURE_FRAMES);
}

static int run_single_case(float freqHz, int bandIndex, float targetDb)
{
    play_dsp_state dsp;
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    float inRms;
    float outRms;
    float measuredDb;
    float err;

    if (play_dsp_init(&dsp, TEST_SAMPLE_RATE, TEST_CHANNELS) != 0)
    {
        fprintf(stderr, "init failed for band %d\n", bandIndex);
        return 1;
    }

    memset(gains, 0, sizeof(gains));
    for (int i = 0; i < EQ_BANDS; ++i)
    {
        qs[i] = 0.9f;
    }
    gains[bandIndex] = targetDb;

    play_dsp_set_eq_params(&dsp, gains, EQ_BANDS, qs, EQ_BANDS);
    play_dsp_set_dynamic_params(&dsp,
                                DYNAMIC_EQ_MIN_ATTACK,
                                DYNAMIC_EQ_MIN_RELEASE,
                                DYNAMIC_EQ_MIN_THRESHOLD,
                                0.0f,
                                DYNAMIC_EQ_MIN_STRENGTH_DB);
    play_dsp_set_multiband_dynamics_config(&dsp,
                                           0,
                                           (uint8_t)PLAY_CROSSFEED_PRE_EQ,
                                           PLAY_MB_DYN_MIN_THRESHOLD,
                                           PLAY_MB_DYN_MIN_ATTACK,
                                           PLAY_MB_DYN_MIN_RELEASE,
                                           PLAY_MB_DYN_MIN_STRENGTH);

    inRms = rms_for_band(&dsp, freqHz, &outRms);
    measuredDb = 20.0f * log10f((outRms + 1e-12f) / (inRms + 1e-12f));
    err = measuredDb - targetDb;

    printf("band=%2d freq=%7.1fHz target=%5.1fdB measured=%7.3fdB err=%+6.3fdB %s\n",
           bandIndex,
           freqHz,
           targetDb,
           measuredDb,
           err,
           (fabsf(err) <= TEST_TOLERANCE_DB) ? "PASS" : "FAIL");

    return (fabsf(err) <= TEST_TOLERANCE_DB) ? 0 : 1;
}

int main(void)
{
    play_dsp_state probe;
    float freqs[EQ_BANDS];
    float gains[EQ_BANDS];
    float qs[EQ_BANDS];
    int failCount = 0;
    int testedBands = 0;

    if (play_dsp_init(&probe, TEST_SAMPLE_RATE, TEST_CHANNELS) != 0)
    {
        fprintf(stderr, "failed to init probe dsp\n");
        return 2;
    }

    play_dsp_copy_eq(&probe, freqs, gains, qs, EQ_BANDS);

    printf("EQ verify: low+mid bands, each band at +/-3dB, tolerance=%.2fdB\n", (double)TEST_TOLERANCE_DB);
    printf("mode split: low<=%.1fHz linear, mid<=%.1fHz dynamic\n", (double)EQ_LINEAR_MAX_HZ, (double)EQ_DYNAMIC_MAX_HZ);

    for (int i = 0; i < EQ_BANDS; ++i)
    {
        float f = freqs[i];
        if (f <= EQ_DYNAMIC_MAX_HZ)
        {
            testedBands += 1;
            failCount += run_single_case(f, i, 3.0f);
            failCount += run_single_case(f, i, -3.0f);
        }
    }

    printf("summary: tested_bands=%d total_cases=%d failed=%d\n",
           testedBands,
           testedBands * 2,
           failCount);

    return (failCount == 0) ? 0 : 1;
}
```

## 9. 埋点设计与反复验证

为满足“EQ 接入前”和“播放前输出”对比需求，DSP 已增加两类帧级埋点：

- EQ 输入埋点：eqTapInSamples
    - 采样位置：每帧在 EQ 算法调用之前（完成前置处理后）
- EQ 输出埋点：eqTapOutSamples
    - 采样位置：每帧在写回播放缓冲之前

并提供导出接口：

- play_dsp_copy_eq_taps(...)

附加时序字段：

- eqTapSeq：每处理一帧 +1
- writeIndex：环形写指针

用途：

1. 完整性验证（是否有漏帧）
2. 准确性验证（采样点是否取对位置）
3. 及时性验证（埋点数据是否与当前帧同步更新）

## 10. 埋点验证测试代码

- 测试程序：src/verify_eq_taps.c
- 构建目标：verify_eq_taps

运行：

```bash
cmake -S . -B build
cmake --build build --target verify_eq_taps -j 4
./verify_eq_taps
```

### 10.1 Case A: Timeliness + Completeness

目标：

- 每处理 1 帧，eqTapSeq 增加 1
- writeIndex 与帧数严格对应
- 序号连续，无跳变

判定：

- seq == frame_count
- seq 单步递增
- writeIndex == frame_count % ANALYZER_WINDOW

### 10.2 Case B: Accuracy (Bypass)

目标：

- 在 bypass=ON 情况下，EQ 不参与处理
- eqTapIn 应等于输入样本
- eqTapOut 应等于最终输出样本

判定：

- |eqTapIn - input| < eps
- |eqTapOut - output| < eps

## 11. 回归测试模板

以下模板用于每次 DSP/EQ 变更后的固定回归流程。

### 11.1 执行命令模板

```bash
# 1) 重新生成构建文件
cmake -S . -B build

# 2) 构建核心目标 + 测试目标
cmake --build build --target play_macos play_mcu verify_eq_gain verify_eq_taps -j 4

# 3) 运行增益一致性测试
./verify_eq_gain

# 4) 运行埋点完整性/准确性/及时性测试
./verify_eq_taps
```

### 11.2 记录模板（可直接复制）

```text
[Regression Date]
YYYY-MM-DD HH:MM

[Build]
play_macos: PASS/FAIL
play_mcu: PASS/FAIL
verify_eq_gain: PASS/FAIL (build)
verify_eq_taps: PASS/FAIL (build)

[Run: verify_eq_gain]
exit_code: 
tested_bands: 
total_cases: 
failed_cases: 
notes: 

[Run: verify_eq_taps]
exit_code: 
case_tap_timeliness_completeness: PASS/FAIL
case_tap_accuracy_bypass: PASS/FAIL
notes: 

[Conclusion]
overall: PASS/FAIL
blocking_issues: 
next_actions: 
```

### 11.3 准入建议

- 必须通过：verify_eq_taps 全部 case
- 建议通过：verify_eq_gain 全部 case
- 若 verify_eq_gain 有失败：需附带失败频点列表与原因分析后再合入

## 12. 三类 EQ 单元测试（./tests/）

本节对应你要求的三类 EQ 函数单测，代码均位于 ./tests/：

- tests/test_eq_linear.c
- tests/test_eq_dynamic.c
- tests/test_eq_normal.c

构建与运行：

```bash
cmake -S . -B build
cmake --build build --target test_eq_linear test_eq_dynamic test_eq_normal -j 4

./test_eq_linear
./test_eq_dynamic
./test_eq_normal

# 或一键执行
cmake --build build --target run_eq_unit_tests -j 4
```

### 12.1 线性 EQ 测试（test_eq_linear）

覆盖函数：

- play_eq_linear_map_gain_db
- play_eq_linear_init
- play_eq_linear_set_config
- play_eq_linear_get_config
- play_eq_linear_process_sample

Case 列表与判定要点：

1. linear_map_plus3_identity / linear_map_minus3_identity
- 目的：验证线性映射函数为恒等映射。
- 判定：输出与输入 dB 误差接近 0。

2. linear_config_enabled_off / linear_config_enabled_on
- 目的：验证 FIR 开关配置可读可写。
- 判定：set 后 get 值一致。

3. linear_config_taps_sanitize_17/25/33
- 目的：验证 taps 规格化逻辑（17/25/33）。
- 判定：输入越界或中间值时，被映射到合法 taps。

4. linear_process_passthrough_disabled
- 目的：验证 FIR 关闭时 process_sample 直通。
- 判定：输出等于输入（在浮点容差内）。

5. linear_rebuild_enabled_with_linear_gain / linear_rebuild_disabled_by_user
- 目的：验证在 DSP 重建链路中，线性段增益可激活 FIR，用户关闭可禁用 FIR。
- 判定：linearFirEnabled 状态符合预期。

### 12.2 动态 EQ 测试（test_eq_dynamic）

覆盖函数：

- play_eq_dynamic_update_env
- play_eq_dynamic_compute_reduction

Case 列表与判定要点：

1. update_env_rise_uses_attack
- 目的：包络上升时使用 attack 系数。
- 判定：env_new = env + attack*(x-env)。

2. update_env_fall_uses_release
- 目的：包络下降时使用 release 系数。
- 判定：env_new = env + release*(x-env)。

3. reduction_requires_positive_gain
- 目的：无正增益场景不能触发动态抑制。
- 判定：结果为 0 dB。

4. reduction_below_threshold_zero
- 目的：低于阈值不触发抑制。
- 判定：结果为 0 dB。

5. reduction_linear_region
- 目的：阈值以上按 (env-threshold)*strength 线性增长。
- 判定：计算值与理论值一致。

6. reduction_clamped_by_max
- 目的：验证最大抑制上限生效。
- 判定：结果不超过 maxReductionDb。

### 12.3 模拟 EQ 测试（test_eq_normal）

覆盖函数：

- play_eq_normal_build_peaking
- play_eq_normal_build_lowpass_12db

Case 列表与判定要点：

1. peaking_0db_unity_at_center
- 目的：0 dB peaking 在中心频点应接近 0 dB。
- 判定：中心频点幅值约等于 1（dB 约 0）。

2. peaking_plus6db_at_center
- 目的：+6 dB peaking 在中心频点应有对应提升。
- 判定：中心频点测得 dB 接近 +6 dB（给定容差）。

3. lowpass_passband_reasonable
- 目的：低通在通带频率应基本保幅。
- 判定：通带 dB 接近 0。

4. lowpass_stopband_attenuates
- 目的：低通在高频应明显衰减。
- 判定：高频 dB 低于设定阈值（如 -8 dB）。

## 13. 回归建议（函数级 + 系统级）

建议采用两层回归：

1. 函数级（快速）
- test_eq_linear / test_eq_dynamic / test_eq_normal

2. 系统级（链路）
- verify_eq_taps：检查埋点完整/准确/及时
- verify_eq_gain：检查频点 +3/-3 dB 实测一致性

推荐提交前命令：

```bash
cmake --build build --target run_eq_unit_tests verify_eq_taps verify_eq_gain -j 4
./verify_eq_taps
./verify_eq_gain
```
