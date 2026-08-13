#include "miniaudio/miniaudio.h"
#include "play_dsp_common.h"
#include "dsp_http_ctrl.h"

#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct
{
    ma_decoder decoder;
} wav_input_source;

static ma_result wav_read_frames(void* userData, float* out, ma_uint64 frameCount, ma_uint64* framesRead)
{
    wav_input_source* source = (wav_input_source*)userData;
    return ma_decoder_read_pcm_frames(&source->decoder, out, frameCount, framesRead);
}

static ma_result wav_rewind(void* userData)
{
    wav_input_source* source = (wav_input_source*)userData;
    return ma_decoder_seek_to_pcm_frame(&source->decoder, 0);
}

static int env_flag_enabled(const char* name)
{
    const char* v = getenv(name);
    if (v == NULL)
    {
        return 0;
    }

    if (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "on") == 0 ||
        strcasecmp(v, "yes") == 0)
    {
        return 1;
    }

    return 0;
}

static void rms_log_accumulate_and_maybe_print(app_audio_state* app,
                                               float* const* planar,
                                               uint32_t frameCount,
                                               uint32_t channels)
{
    uint32_t chn;

    if (app == NULL || planar == NULL || !app->rmsLogEnabled || app->rmsLogWindowFrames == 0u)
    {
        return;
    }

    chn = (channels > MAX_CHANNELS) ? MAX_CHANNELS : channels;
    for (uint32_t ch = 0; ch < chn; ++ch)
    {
        for (uint32_t i = 0; i < frameCount; ++i)
        {
            double s = (double)planar[ch][i];
            app->rmsLogSumSq[ch] += s * s;
        }
    }

    app->rmsLogFrameAccum += frameCount;
    if (app->rmsLogFrameAccum < app->rmsLogWindowFrames)
    {
        return;
    }

    if (chn >= 2u)
    {
        double invN = 1.0 / (double)app->rmsLogFrameAccum;
        double rmsL = sqrt(app->rmsLogSumSq[0] * invN);
        double rmsR = sqrt(app->rmsLogSumSq[1] * invN);
        printf("[RMS] window=%u L=%.6f R=%.6f\n", app->rmsLogFrameAccum, rmsL, rmsR);
    }
    else if (chn == 1u)
    {
        double invN = 1.0 / (double)app->rmsLogFrameAccum;
        double rms = sqrt(app->rmsLogSumSq[0] * invN);
        printf("[RMS] window=%u M=%.6f\n", app->rmsLogFrameAccum, rms);
    }

    app->rmsLogFrameAccum = 0u;
    for (uint32_t ch = 0; ch < MAX_CHANNELS; ++ch)
    {
        app->rmsLogSumSq[ch] = 0.0;
    }
}

static void audio_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount)
{
    app_audio_state* app = (app_audio_state*)pDevice->pUserData;
    float* out = (float*)pOutput;
    uint32_t pipelineChannels;
    float planarStorage[MAX_CHANNELS][frameCount > 0u ? frameCount : 1u];
    float* planarPtrs[MAX_CHANNELS] = {0};
    ma_uint64 totalRead = 0;
    int rewindAttempts = 0;

    (void)pInput;

    while (totalRead < frameCount)
    {
        ma_uint64 chunkRead = 0;
        ma_result readResult = app->input.read_frames(app->input.userData, out + totalRead * app->channels,
                                                      frameCount - totalRead, &chunkRead);

        if (readResult != MA_SUCCESS || chunkRead == 0)
        {
            if (rewindAttempts >= 3 || app->input.rewind(app->input.userData) != MA_SUCCESS)
            {
                memset(out + totalRead * app->channels, 0,
                       (size_t)((frameCount - totalRead) * app->channels * sizeof(float)));
                break;
            }
            rewindAttempts += 1;
            continue;
        }

        totalRead += chunkRead;
        rewindAttempts = 0;
    }

    pipelineChannels = (app->channels > MAX_CHANNELS) ? MAX_CHANNELS : app->channels;
    for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
    {
        planarPtrs[ch] = planarStorage[ch];
    }

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            planarStorage[ch][i] = out[i * app->channels + ch];
        }
    }

    pthread_mutex_lock(&app->dspMutex);
    play_dsp_process_planar(&app->dsp, planarPtrs, frameCount, pipelineChannels);
    rms_log_accumulate_and_maybe_print(app, planarPtrs, frameCount, pipelineChannels);
    pthread_mutex_unlock(&app->dspMutex);

    for (uint32_t i = 0; i < frameCount; ++i)
    {
        for (uint32_t ch = 0; ch < pipelineChannels; ++ch)
        {
            out[i * app->channels + ch] = planarStorage[ch][i];
        }
    }
}

const char* input_audio_file_path = "1.wav";

int main(void)
{
    ma_result result;
    ma_device device;
    ma_device_config deviceConfig;
    ma_decoder_config decoderConfig;
    wav_input_source wavSource;
    app_audio_state app;
    http_server_state http;

    memset(&app, 0, sizeof(app));
    memset(&http, 0, sizeof(http));
    memset(&wavSource, 0, sizeof(wavSource));
    http.serverFd = -1;

    deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format = ma_format_f32;
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate = 48000;
    deviceConfig.dataCallback = audio_data_callback;
    deviceConfig.pUserData = &app;

    result = ma_device_init(NULL, &deviceConfig, &device);
    if (result != MA_SUCCESS)
    {
        printf("初始化播放设备失败。\n");
        return -1;
    }

    decoderConfig = ma_decoder_config_init(ma_format_f32, device.playback.channels, device.sampleRate);

    result = ma_decoder_init_file(input_audio_file_path, &decoderConfig, &wavSource.decoder);
    if (result != MA_SUCCESS)
    {
        printf("加载 wav 失败。\n");
        ma_device_uninit(&device);
        return -1;
    }

    app.channels = device.playback.channels;
    app.rmsLogEnabled = env_flag_enabled("PLAY_RMS_LOG") ? 1u : 0u;
    app.rmsLogWindowFrames = (device.sampleRate >= 4u) ? (device.sampleRate / 4u) : 1u;
    app.rmsLogFrameAccum = 0u;
    app.rmsLogSumSq[0] = 0.0;
    app.rmsLogSumSq[1] = 0.0;
    app.input.userData = &wavSource;
    app.input.read_frames = wav_read_frames;
    app.input.rewind = wav_rewind;
    if (pthread_mutex_init(&app.dspMutex, NULL) != 0)
    {
        printf("初始化互斥锁失败。\n");
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }
    if (play_dsp_init(&app.dsp, device.sampleRate, app.channels) != 0)
    {
        printf("初始化 DSP 失败。\n");
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    http.app = &app;
    atomic_init(&http.running, true);
    if (pthread_create(&http.thread, NULL, dsp_http_ctrl_thread_main, &http) != 0)
    {
        printf("启动 HTTP 线程失败。\n");
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS)
    {
        printf("启动播放设备失败。\n");
        atomic_store_explicit(&http.running, false, memory_order_release);
        if (http.serverFd >= 0)
        {
            shutdown(http.serverFd, SHUT_RDWR);
        }
        pthread_join(http.thread, NULL);
        pthread_mutex_destroy(&app.dspMutex);
        ma_device_uninit(&device);
        ma_decoder_uninit(&wavSource.decoder);
        return -1;
    }

    printf("正在循环播放 test.wav（低频 low-seq + 现有 EQ 链路）。\n");
    printf("频谱可视化: http://127.0.0.1:%d\n", HTTP_PORT);
    printf("RMS 日志: %s（POST /eq: {\"rmsLogEnabled\":0|1}，或环境变量 PLAY_RMS_LOG=1）\n",
           app.rmsLogEnabled ? "ON" : "OFF");
    printf("按回车键退出。\n");
    getchar();

    atomic_store_explicit(&http.running, false, memory_order_release);
    if (http.serverFd >= 0)
    {
        shutdown(http.serverFd, SHUT_RDWR);
    }
    pthread_join(http.thread, NULL);

    ma_device_uninit(&device);
    ma_decoder_uninit(&wavSource.decoder);
    pthread_mutex_destroy(&app.dspMutex);

    return 0;
}
