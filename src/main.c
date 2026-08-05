#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <stdio.h>

// 定义 EQ 频段数量
#define EQ_BANDS 3

// 音频引擎全局结构体
typedef struct {
    ma_decoder decoder;
    ma_lpf lpf;                         // 低通滤波器（用于分频/滤波）
    ma_biquad eq_bands[EQ_BANDS];       // 3段 EQ (使用双二阶滤波器)
} AudioEngine;

// 音频播放回调函数（实时处理音频数据）
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    AudioEngine* pEngine = (AudioEngine*)pDevice->pUserData;
    if (pEngine == NULL) return;

    // 1. 从 WAV 解码器读取原始音频数据
    ma_uint32 framesRead = (ma_uint32)ma_decoder_read_pcm_frames(&pEngine->decoder, pOutput, frameCount, NULL);

    // 2. 实时滤波处理 (Low Pass Filter)
    ma_lpf_process_pcm_frames(&pEngine->lpf, pOutput, pOutput, framesRead);

    // 3. 实时多频段 EQ 处理
    for (int i = 0; i < EQ_BANDS; ++i) {
        ma_biquad_process_pcm_frames(&pEngine->eq_bands[i], pOutput, pOutput, framesRead);
    }

    // 如果播放完毕，自动循环或停止
    if (framesRead < frameCount) {
        ma_decoder_seek_to_pcm_frame(&pEngine->decoder, 0); // 循环播放
    }
}

int main() {
    ma_result result;
    AudioEngine engine;
    ma_device_config deviceConfig;
    ma_device device;

    // 1. 初始化 WAV 解码器
    // 请将 "test.wav" 替换为您的本地测试音频路径
    result = ma_decoder_init_file("test.wav", NULL, &engine.decoder);
    if (result != MA_SUCCESS) {
        printf("无法打开 WAV 文件\n");
        return -1;
    }

    ma_uint32 sampleRate = engine.decoder.outputSampleRate;
    ma_uint32 channels = engine.decoder.outputChannels;

    // 2. 初始化低通滤波器 (例如：裁剪掉 4000Hz 以上的高频)
    // 嵌入式分频核心：通过配置不同的 LPF(低通) 和 HPF(高通) 即可实现数字分频
    ma_lpf_config lpfConfig = ma_lpf_config_init(engine.decoder.outputFormat, channels, sampleRate, 4000.0, 3);
    ma_lpf_init(&lpfConfig, NULL, &engine.lpf);

    // 3. 初始化 3 段 EQ (使用高阶双二阶 Peaking 滤波器)
    // 频段 1: 低音 (100Hz), 增益 +6dB, 带宽(Q值) 1.0
    ma_biquad_config eq1 = ma_biquad_config_init(engine.decoder.outputFormat, channels, ma_biquad_type_peaking, sampleRate);
    eq1.peaking.frequency = 100.0; eq1.peaking.gainDB = 6.0; eq1.peaking.q = 1.0;
    ma_biquad_init(&eq1, &engine.eq_bands[0]);

    // 频段 2: 中音 (1000Hz), 增益 -3dB, 带宽(Q值) 1.0
    ma_biquad_config eq2 = ma_biquad_config_init(engine.decoder.outputFormat, channels, ma_biquad_type_peaking, sampleRate);
    eq2.peaking.frequency = 1000.0; eq2.peaking.gainDB = -3.0; eq2.peaking.q = 1.0;
    ma_biquad_init(&eq2, &engine.eq_bands[1]);

    // 频段 3: 高音 (10000Hz), 增益 +4dB, 带宽(Q值) 1.0
    ma_biquad_config eq3 = ma_biquad_config_init(engine.decoder.outputFormat, channels, ma_biquad_type_peaking, sampleRate);
    eq3.peaking.frequency = 10000.0; eq3.peaking.gainDB = 4.0; eq3.peaking.q = 1.0;
    ma_biquad_init(&eq3, &engine.eq_bands[2]);

    // 4. 配置并启动 macOS 播放设备
    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = engine.decoder.outputFormat;
    deviceConfig.playback.channels = channels;
    deviceConfig.sampleRate        = sampleRate;
    deviceConfig.dataCallback      = data_callback;
    deviceConfig.pUserData         = &engine;

    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        printf("无法初始化播放设备\n");
        ma_decoder_uninit(&engine.decoder);
        return -1;
    }

    // 启动音频播放
    ma_device_start(&device);

    printf("正在播放... 按回车键退出。\n");
    getchar();

    // 5. 清理资源
    ma_device_uninit(&device);
    ma_decoder_uninit(&engine.decoder);
    return 0;
}
