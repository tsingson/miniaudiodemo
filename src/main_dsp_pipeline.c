#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <unistd.h>
#define sleep_ms(ms) usleep((ms) * 1000)
#endif

#include "miniaudio.h"
#include "dsp_pipeline.h"



void pipeline_init(DspPipeline* pipeline, ma_uint32 sampleRate, ma_uint32 channels) {
    pipeline->context.sampleRate = sampleRate;
    pipeline->context.channels = channels;
    pipeline->nodeCount = 0;
    memset(pipeline->nodes, 0, sizeof(pipeline->nodes));
}

// 向流水线追加节点
bool pipeline_append_node(DspPipeline* pipeline, const DspNodeVTable* vtable, void* instanceData) {
    if (pipeline->nodeCount >= MAX_DSP_NODES) {
        return false; // 流水线已满
    }

    ma_uint32 index = pipeline->nodeCount;
    pipeline->nodes[index].vtable = vtable;
    pipeline->nodes[index].instanceData = instanceData;

    // 立即初始化该节点
    if (vtable->onInit) {
        vtable->onInit(instanceData, &pipeline->context);
    }

    pipeline->nodeCount++;
    return true;
}

// 执行流水线（在 miniaudio 驱动线程中被安全、无阻塞地调用）
void pipeline_process(DspPipeline* pipeline, float* pFrames, ma_uint32 frameCount) {
    for (ma_uint32 i = 0; i < pipeline->nodeCount; ++i) {
        DspNode* node = &pipeline->nodes[i];
        if (node->vtable && node->vtable->onProcess) {
            node->vtable->onProcess(node->instanceData, pFrames, frameCount);
        }
    }
}



// miniaudio 硬件音频数据回调
void ma_audio_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    DspPipeline* pipeline = (DspPipeline*)pDevice->pUserData;
    if (!pipeline || !pOutput) return;

    // 强转为连续的平面 float 指针进行 DSP 链式就地（In-place）处理
    float* pFloatOutput = (float*)pOutput;

    /*
       [业务拓展步]：如果是音频播放器，在此处将音频解码源数据读入到 pFloatOutput 中
       ma_decoder_read_pcm_frames(&decoder, pFloatOutput, frameCount, NULL);
    */

    // 喂入 DSP 流水线修改数据
    pipeline_process(pipeline, pFloatOutput, frameCount);
}

int main() {
    // 1. 初始化本地数据实体（在栈上或全局分配，严格禁止在音频线程分配）
    DspPipeline pipeline;
    GainNodeData volumeNodeData = {
        .currentGain = 1.0f,
        .smoothingFactor = 0.01f,
        .channels = 2
    };
    atomic_init(&volumeNodeData.targetGain, 1.0f); // C11/C17 原子初始化

    // 2. 初始化流水线基础配置
    pipeline_init(&pipeline, 48000, 2);

    // 3. 将装配好的节点注册进流水线
    if (!pipeline_append_node(&pipeline, &g_GainNodeVTable, &volumeNodeData)) {
        printf("Failed to append DSP node.\n");
        return -1;
    }

    // 4. 配置 miniaudio 设备
    ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
    deviceConfig.playback.format   = ma_format_f32; // 工业规范：强制内部处理使用 float32
    deviceConfig.playback.channels = 2;
    deviceConfig.sampleRate        = 48000;
    deviceConfig.dataCallback      = ma_audio_callback;
    deviceConfig.pUserData         = &pipeline;

    ma_device device;
    if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
        printf("Failed to initialize miniaudio device.\n");
        return -1;
    }

    // 确保流水线和硬件的采样率、通道数完全同步
    pipeline_init(&pipeline, device.sampleRate, device.playback.channels);

    // 5. 启动音频硬件线程
    if (ma_device_start(&device) != MA_SUCCESS) {
        printf("Failed to start audio device.\n");
        ma_device_uninit(&device);
        return -1;
    }

    printf("C17 Audio Pipeline is running. Testing smooth volume transition...\n");

    // 主线程业务代码：无锁、非阻塞地安全调整参数，音频线程内部会自动完成平滑淡出
    gain_node_set_gain(&volumeNodeData, 0.1f);
    sleep_ms(2000);

    // 平滑淡入
    gain_node_set_gain(&volumeNodeData, 0.8f);
    sleep_ms(2000);

    // 6. 清理退出
    ma_device_uninit(&device);
    printf("Pipeline shut down cleanly.\n");
    return 0;
}
