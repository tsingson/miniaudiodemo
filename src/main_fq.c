#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio/miniaudio.h"
#ifdef USE_CMSIS_DSP
#include "arm_math.h"
#endif

#include <math.h>
#include <string.h>
#include <stdio.h>

// --- 配置常量 ---
#define SAMPLE_RATE         48000 // 原始采样率
#define BLOCK_SIZE          32    // 每次处理的采样点数（必须是 32 的整数倍）
#define DECIMATION_FACTOR   32    // 降采样因子 (48kHz -> 1.5kHz)
#define FIR_TAP_COUNT       192   // 核心低频 EQ FIR 阶数
#define ANTI_ALIAS_TAPS     64    // 抗混叠/镜像滤波器阶数

// --- 滤波器系数 ---
// 1. 核心低频 EQ FIR 滤波器系数（1.5kHz 采样率下的线性相位响应）
const float32_t firCoeffs[FIR_TAP_COUNT] = {
    -0.000017f, 0.000012f, 0.000007f, -0.000004f, 0.000006f, -0.000003f, -0.000002f, -0.000021f, -0.000015f, -0.000009f,
    -0.000057f, -0.000008f, -0.000010f, -0.000075f, 0.000036f, -0.000006f, -0.000071f, 0.000097f, -0.000023f,
    -0.000063f, 0.000136f, -0.000082f, -0.000055f, 0.000132f, -0.000159f, -0.000036f, 0.000090f, -0.000217f, -0.000014f,
    0.000009f, -0.000232f, -0.000021f, -0.000076f, -0.000164f, -0.000043f, -0.000063f, 0.000023f, -0.000071f, 0.000092f,
    0.000218f, -0.000214f, 0.000264f, 0.000155f, -0.000593f, 0.000336f, -0.000255f, -0.001023f, 0.000460f, -0.000739f,
    -0.001067f, 0.000840f, -0.001015f, -0.000577f, 0.001271f, -0.001165f, 0.000081f, 0.001277f, -0.001338f, 0.000496f,
    0.000732f, -0.001355f, 0.000468f, -0.000101f, -0.001086f, -0.000355f, -0.000992f, -0.000814f, -0.002315f,
    -0.001334f, -0.000519f, -0.004668f, 0.000535f, 0.000434f, -0.005879f, 0.005879f, 0.001211f, -0.006206f, 0.012679f,
    -0.002519f, -0.008560f, 0.016347f, -0.015756f, -0.013900f, 0.015416f, -0.038310f, -0.015668f, 0.014890f, -0.065229f,
    -0.000627f, 0.020588f, -0.099472f, 0.053901f, 0.030477f, -0.196605f, 0.463179f, 1.140402f, 0.463179f, -0.196605f,
    0.030477f, 0.053901f, -0.099472f, 0.020588f, -0.000627f, -0.065229f, 0.014890f, -0.015668f, -0.038310f, 0.015416f,
    -0.013900f, -0.015756f, 0.016347f, -0.008560f, -0.002519f, 0.012679f, -0.006206f, 0.001211f, 0.005879f, -0.005879f,
    0.000434f, 0.000535f, -0.004668f, -0.000519f, -0.001334f, -0.002315f, -0.000814f, -0.000992f, -0.000355f,
    -0.001086f, -0.000101f, 0.000468f, -0.001355f, 0.000732f, 0.000496f, -0.001338f, 0.001277f, 0.000081f, -0.001165f,
    0.001271f, -0.000577f, -0.001015f, 0.000840f, -0.001067f, -0.000739f, 0.000460f, -0.001023f, -0.000255f, 0.000336f,
    -0.000593f, 0.000155f, 0.000264f, -0.000214f, 0.000218f, 0.000092f, -0.000071f, 0.000023f, -0.000063f, -0.000043f,
    -0.000164f, -0.000076f, -0.000021f, -0.000232f, 0.000009f, -0.000014f, -0.000217f, 0.000090f, -0.000036f,
    -0.000159f, 0.000132f, -0.000055f, -0.000082f, 0.000136f, -0.000063f, -0.000023f, 0.000097f, -0.000071f, -0.000006f,
    0.000036f, -0.000075f, -0.000010f, -0.000008f, -0.000057f, -0.000009f, -0.000015f, -0.000021f, -0.000002f,
    -0.000003f, 0.000006f, -0.000004f, 0.000007f, 0.000012f, -0.000017f
};

// 2. 降采样/升采样抗混叠 FIR 滤波器系数（48kHz 采样率，截止频率约 600Hz）
const float32_t antiAliasCoeffs[ANTI_ALIAS_TAPS] = {
    0.001230f, 0.003420f, 0.005410f, // ...标准低通滤波器系数...
};

// --- CMSIS-DSP 状态缓冲区 ---
static float32_t decimatorState[BLOCK_SIZE + ANTI_ALIAS_TAPS - 1];
static float32_t interpolatorState[BLOCK_SIZE + ANTI_ALIAS_TAPS - 1];
static float32_t firEQState[(BLOCK_SIZE / DECIMATION_FACTOR) + FIR_TAP_COUNT - 1];

// --- CMSIS-DSP 实例结构体 ---
arm_fir_decimate_instance_f32 Decimator;
arm_fir_interpolate_instance_f32 Interpolator;
arm_fir_instance_f32 LowFreqEQ;

// --- 内部中间流缓冲区 ---
float32_t downsampledBuffer[BLOCK_SIZE / DECIMATION_FACTOR];
float32_t processedBuffer[BLOCK_SIZE / DECIMATION_FACTOR];

// --- 音频处理初始化 ---
void AudioProcessing_Init(void)
{
    // 1. 初始化降采样器 (48kHz -> 1.5kHz)
    arm_fir_decimate_init_f32(&Decimator, ANTI_ALIAS_TAPS, DECIMATION_FACTOR,
                              (float32_t*)&antiAliasCoeffs, decimatorState, BLOCK_SIZE);

    // 2. 初始化核心 0 相位低频 FIR EQ
    arm_fir_init_f32(&LowFreqEQ, FIR_TAP_COUNT, (float32_t*)&firCoeffs,
                     firEQState, (BLOCK_SIZE / DECIMATION_FACTOR));

    // 3. 初始化升采样器 (1.5kHz -> 48kHz)
    arm_fir_interpolate_init_f32(&Interpolator, DECIMATION_FACTOR, ANTI_ALIAS_TAPS,
                                 (float32_t*)&antiAliasCoeffs, interpolatorState, (BLOCK_SIZE / DECIMATION_FACTOR));
}

// --- 核心音频处理块（针对 32 个单声道采样点） ---
void Process_Audio_Block(float32_t* pInput, float32_t* pOutput)
{
    // 步骤 1: 抗混叠低通滤波并降采样 (32点 @ 48kHz -> 1点 @ 1.5kHz)
    arm_fir_decimate_f32(&Decimator, pInput, downsampledBuffer, BLOCK_SIZE);

    // 步骤 2: 在 1.5kHz 低采样率下进行高分辨率、无相位失真的 EQ 处理
    arm_fir_f32(&LowFreqEQ, downsampledBuffer, processedBuffer, (BLOCK_SIZE / DECIMATION_FACTOR));

    // 步骤 3: 升采样并滤除高频镜像 (1点 @ 1.5kHz -> 32点 @ 48kHz)
    arm_fir_interpolate_f32(&Interpolator, processedBuffer, pOutput, (BLOCK_SIZE / DECIMATION_FACTOR));
}

// --- Miniaudio 数据回调函数 ---
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    // 确保我们只处理单声道（Mono）的浮点数据
    float32_t* pInFloat = (float32_t*)pInput;
    float32_t* pOutFloat = (float32_t*)pOutput;

    // 这里的 frameCount 是 miniaudio 传进来的总帧数
    // 我们需要按 BLOCK_SIZE (32) 为一组切片进行循环处理
    ma_uint32 processedFrames = 0;
    while (processedFrames < frameCount)
    {
        // 如果最后一组数据不够 BLOCK_SIZE，直接拷贝不处理（或进行零填充，此处做简单拷贝）
        if ((frameCount - processedFrames) < BLOCK_SIZE)
        {
            for (ma_uint32 i = 0; i < (frameCount - processedFrames); ++i)
            {
                pOutFloat[processedFrames + i] = pInFloat[processedFrames + i];
            }
            break;
        }

        // 调用刚才的 CMSIS-DSP 低频 EQ 处理管道
        Process_Audio_Block(&pInFloat[processedFrames], &pOutFloat[processedFrames]);

        processedFrames += BLOCK_SIZE;
    }
}

// --- 主函数 ---
int main()
{
    // 1. 初始化音频算法状态
    AudioProcessing_Init();
    printf("CMSIS-DSP 低频 EQ 算法初始化成功...\n");

    // 2. 配置 Miniaudio 设备
    ma_device_config deviceConfig;
    // 选择全工模式（同时打开输入录音和输出播放）
    deviceConfig = ma_device_config_init(ma_device_type_duplex);
    deviceConfig.capture.format = ma_format_f32; // 必须使用 32位浮点数 对应 CMSIS-DSP
    deviceConfig.capture.channels = 1; // 单声道处理
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = 1;
    deviceConfig.sampleRate = SAMPLE_RATE; // 固定 48000 Hz
    deviceConfig.dataCallback = data_callback; // 绑定回调函数
    deviceConfig.periodSizeInFrames = BLOCK_SIZE; // 建议让 miniaudio 缓冲区对齐 BLOCK_SIZE

    // 3. 启动音频设备
    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS)
    {
        printf("无法初始化 Miniaudio 音频设备。\n");
        return -1;
    }

    if (ma_device_start(&device) != MA_SUCCESS)
    {
        printf("无法启动音频流。\n");
        ma_device_uninit(&device);
        return -1;
    }

    printf("低频无失真 EQ 正在运行。按回车键退出...\n");
    getchar();

    // 4. 清理并退出
    ma_device_uninit(&device);
    return 0;
}
