#include <math.h>
#include <stdlib.h>
#include "miniaudio.h"
#include "dsp_pipeline.h"

// 增益节点私有数据
typedef struct
{
    _Atomic float targetGain; // 线程安全的原子目标值
    float currentGain; // 当前平滑值（仅音频线程访问）
    float smoothingFactor;
    ma_uint32 channels;
} GainNodeData;

// 增益节点 - 初始化
static void gain_onInit(void* instance, const DspContext* context)
{
    GainNodeData* data = (GainNodeData*)instance;
    data->channels = context->channels;

    // 50ms 平滑时间常数
    float timeConstant = 0.050f;
    data->smoothingFactor = 1.0f - expf(-1.0f / (context->sampleRate * timeConstant));
}

// 增益节点 - 核心音频处理（交错式 Interleaved 内存处理）
static void gain_onProcess(void* instance, float* pFrames, ma_uint32 frameCount)
{
    GainNodeData* data = (GainNodeData*)instance;

    // 使用 memory_order_relaxed 获取原子变量，避免由于 CPU 缓存同步导致的流水线停顿
    float target = atomic_load_explicit(&data->targetGain, memory_order_relaxed);
    ma_uint32 totalSamples = frameCount * data->channels;

    // 快路径：如果增益已经稳定，直接执行高度向量化的循环
    if (fabsf(data->currentGain - target) < 1e-5f)
    {
        data->currentGain = target;
        for (ma_uint32 i = 0; i < totalSamples; ++i)
        {
            pFrames[i] *= target;
        }
    }
    else
    {
        // 慢路径：逐帧进行线性/指数平滑，防止爆音
        ma_uint32 sampleIndex = 0;
        for (ma_uint32 f = 0; f < frameCount; ++f)
        {
            data->currentGain += (target - data->currentGain) * data->smoothingFactor;

            for (ma_uint32 c = 0; c < data->channels; ++c)
            {
                pFrames[sampleIndex++] *= data->currentGain;
            }
        }
    }
}

static const char* gain_getName(void* instance)
{
    (void)instance;
    return "VolumeGainNode";
}

// 导出虚函数表
static const DspNodeVTable g_GainNodeVTable = {
    .onInit = gain_onInit,
    .onProcess = gain_onProcess,
    .getName = gain_getName
};

// 供主线程调用的外部 API：安全修改音量
void gain_node_set_gain(GainNodeData* data, float newGain)
{
    atomic_store_explicit(&data->targetGain, newGain, memory_order_relaxed);
}


