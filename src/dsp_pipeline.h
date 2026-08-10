#ifndef DSP_PIPELINE_H
#define DSP_PIPELINE_H

#include "miniaudio.h"
#include <stdatomic.h>
#include <stdbool.h>

#define MAX_DSP_NODES 16

// DSP 节点共享上下文
typedef struct {
    ma_uint32 sampleRate;
    ma_uint32 channels;
} DspContext;

// 虚函数表：定义 DSP 节点的统一行为
typedef struct DspNodeVTable {
    void (*onInit)(void* instance, const DspContext* context);
    void (*onProcess)(void* instance, float* pFrames, ma_uint32 frameCount);
    const char* (*getName)(void* instance);
} DspNodeVTable;

// DSP 节点基类结构体
typedef struct {
    const DspNodeVTable* vtable;
    void* instanceData; // 指向具体节点实现的数据（如 GainData, FilterData）
} DspNode;

// DSP 流水线管理器
typedef struct {
    DspContext context;
    DspNode nodes[MAX_DSP_NODES];
    ma_uint32 nodeCount;
} DspPipeline;

#endif // DSP_PIPELINE_H
