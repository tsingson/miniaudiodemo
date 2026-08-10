#include "miniaudio/miniaudio.h"
#include <stdio.h>

typedef struct
{
    ma_decoder decoder;
} AudioEngine;

// 处理音频数据的回调函数
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    AudioEngine* pEngine = (AudioEngine*)pDevice->pUserData;

    if (pEngine == NULL) return;

    ma_uint64 framesRead = 0;
    ma_decoder_read_pcm_frames(&pEngine->decoder, pOutput, frameCount, &framesRead);

    if (framesRead < frameCount)
    {
        ma_decoder_seek_to_pcm_frame(&pEngine->decoder, 0); // 循环播放
    }
}

int main()
{
    ma_result result;
    AudioEngine engine;
    ma_device_config deviceConfig = {0};
    ma_device device;

    const char* filePath = "test.wav";
    if ((result = ma_decoder_init_file(filePath, NULL, &engine.decoder)) != MA_SUCCESS)
    {
        printf("无法打开 WAV 文件 %s (错误码: %d)\n", filePath, result);
        return -1;
    }
    printf("已加载: %s  格式=%d 声道=%u 采样率=%u\n", filePath,
           engine.decoder.outputFormat, engine.decoder.outputChannels, engine.decoder.outputSampleRate);

    ma_uint32 sampleRate = engine.decoder.outputSampleRate;
    ma_uint32 channels = engine.decoder.outputChannels;

    // 配置播放设备
    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = engine.decoder.outputFormat;
    deviceConfig.playback.channels = channels;
    deviceConfig.sampleRate = sampleRate;
    deviceConfig.dataCallback = data_callback;
    deviceConfig.pUserData = &engine;

    // 初始化播放设备
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS)
    {
        printf("无法初始化播放设备\n");
        ma_decoder_uninit(&engine.decoder);
        return -1;
    }

    // 启动音频播放
    if ((result = ma_device_start(&device)) != MA_SUCCESS)
    {
        printf("无法启动播放设备\n");
        ma_device_uninit(&device);
        ma_decoder_uninit(&engine.decoder);
        return -1;
    }

    printf("正在播放... 按回车键退出。\n");
    getchar();

    // 清理资源
    ma_device_uninit(&device);
    ma_decoder_uninit(&engine.decoder);

    return 0;
}
