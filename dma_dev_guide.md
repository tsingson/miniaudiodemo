# STM32H7B0 DMA 双缓冲开发指南

## 1. DMA 双缓冲方案概述

### 1.1 概念介绍
DMA 双缓冲方案通过两个独立的 DMA 缓冲区交替使用，实现音频数据的高速传输和连续播放。该方案的核心思想是：当一个缓冲区被 DMA 传输完成时，另一个缓冲区已经准备好用于下一次传输，从而实现无缝的音频流。

### 1.2 工作原理
```
+----------------+    +----------------+    +----------------+
| 缓冲区 1      |    | 缓冲区 2      |    | 缓冲区 1      |
| (传输中)      | -> | (准备中)      | -> | (传输完成)    |
+----------------+    +----------------+    +----------------+
     ^                       ^                       ^
     |                       |                       |
DMA 完成              DMA 开始              DMA 完成
```

### 1.3 应用场景
- 音频播放（PCM5102A DAC 驱动）
- USB UAC2 音频采集
- DSP 音频处理
- 多声道音频同步

## 2. STM32H7B0 DMA 控制器特性

### 2.1 硬件特性
- **DMA 通道数**: 12 个独立 DMA 通道
- **支持外设**: I2S、SPI、UART、ADC、DAC 等
- **缓冲大小**: 支持 1 字节到 65536 字节的传输
- **传输模式**: 内存到外设、外设到内存、内存到内存
- **中断支持**: 传输完成、半传输、传输错误

### 2.2 DMA 寄存器关键配置
```c
// DMA 控制寄存器 (DMA_CCR)
#define DMA_CCR_EN        (1 << 0)    // DMA 通道使能
#define DMA_CCR_TCIE      (1 << 1)    // 传输完成中断使能
#define DMA_CCR_HTIE      (1 << 2)    // 半传输中断使能
#define DMA_CCR_TEIE      (1 << 3)    // 传输错误中断使
#define DMA_CCR_DIR_0     (0 << 4)    // 方向位 0
#define DMA_CCR_DIR_1     (1 << 5)    // 方向位 1
#define DMA_CCR_PINC      (1 << 6)    // 外设地址增量
#define DMA_CCR_MINC      (1 << 7)    // 内存地址增量
#define DMA_CCR_PSIZE_0   (0 << 8)    // 外设数据宽度 0
#define DMA_CCR_PSIZE_1   (1 << 9)    // 外设数据宽度 1
#define DMA_CCR_MSIZE_0   (0 << 10)   // 内存数据宽度 0
#define DMA_CCR_MSIZE_1   (1 << 11)   // 内存数据宽度 1
#define DMA_CCR_PL_0      (0 << 12)   // 优先级级别 0
#define DMA_CCR_PL_1      (1 << 13)   // 优先级级别 1
#define DMA_CCR_MBURST    (1 << 16)   // 内存突发传输
#define DMA_CCR_PBURST    (1 << 18)   // 外设突发传输
#define DMA_CCR_CT        (1 << 22)   // 循环模式
#define DMA_CCR_DBM       (1 << 23)   // 双缓冲模式
#define DMA_CCR_PFCTRL    (1 << 24)   // 外设固定地址

// DMA 流传输寄存器 (DMA_CNDTR)
#define DMA_CNDTR_NDT    (0xFFFF << 0) // 传输字节数

// DMA 外围设备地址寄存器 (DMA_CPAR)
#define DMA_CPAR_PA      (0xFFFFFFFF << 0) // 外设地址

// DMA 内存地址寄存器 (DMA_CMAR)
#define DMA_CMAR_MA      (0xFFFFFFFF << 0) // 内存地址
```

## 3. STM32H7B0 I2S DMA 双缓冲实现

### 3.1 I2S 外设特性
- **采样率**: 支持 48kHz、96kHz 等
- **数据宽度**: 16 位、24 位、32 位
- **通道数**: 单声道、双声道
- **DMA 传输**: 支持 DMA 连续传输

### 3.2 DMA 双缓冲配置步骤

#### 3.2.1 缓冲区分配
```c
#define DMA_BUFFER_SIZE     (4096)  // 每个缓冲区大小
uint32_t dma_buffer1[DMA_BUFFER_SIZE];
uint32_t dma_buffer2[DMA_BUFFER_SIZE];
uint32_t *current_buffer = dma_buffer1;
```

#### 3.2.2 DMA 通道初始化
```c
void configure_dma_double_buffer(void) {
    // 1. 配置 DMA 通道
    DMA_InitTypeDef dma_init;
    
    // 2. 配置 DMA 控制寄存器
    dma_init.DMA_Channel = DMA_CHANNEL_I2S_TX;
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&I2S1->TXDR;
    dma_init.DMA_Memory0BaseAddr = (uint32_t)current_buffer;
    dma_init.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    dma_init.DMA_BufferSize = DMA_BUFFER_SIZE;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma_init.DMA_Mode = DMA_Mode_Normal;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    dma_init.DMA_DoubleBufferMode = DMA_DoubleBufferMode_Enable;
    dma_init.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    dma_init.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    
    // 3. 初始化 DMA 通道
    HAL_DMA_Init(&hdma_i2s_tx);
    
    // 4. 配置双缓冲地址
    HAL_DMAEx_ConfigDoubleBufferMode(&hdma_i2s_tx, 
                                     (uint32_t)current_buffer,
                                     (uint32_t)current_buffer == (uint32_t)dma_buffer1 ? 
                                         (uint32_t)dma_buffer2 : (uint32)
```

#### 3.2.3 中断处理
```c
void HAL_DMA_XferHalfCpltCallback(DMA_HandleTypeDef *hdma) {
    // 半传输完成回调
    if (hdma->Instance == DMA1_Stream5) {  // I2S TX DMA
        // 切换到下一个缓冲区
        current_buffer = (current_buffer == dma_buffer1) ? 
                         dma_buffer2 : dma_buffer1;
        
        // 重新加载 DMA 缓冲区地址
        __HAL_DMA_DISABLE(hdma);
        hdma->Instance->M0AR = (uint32_t)current_buffer;
        __HAL_DMA_ENABLE(hdma);
        
        // 重新填充缓冲区数据
        fill_dma_buffer(current_buffer, DMA_BUFFER_SIZE);
    }
}

void HAL_DMA_XferCpltCallback(DMA_HandleTypeDef *hdma) {
    // 传输完成回调
    if (hdma->Instance == DMA1_Stream5) {
        // 切换到下一个缓冲区
        current_buffer = (current_buffer == dma_buffer1) ? 
                         dma_buffer2 : dma_buffer1;
        
        // 重新加载 DMA 缓冲区地址
        __HAL_DMA_DISABLE(hdma);
        hdma->Instance->M0AR = (uint32_t)current_buffer;
        __HAL_DMA_ENABLE(hdma);
        
        // 重新填充缓冲区数据
        fill_dma_buffer(current_buffer, DMA_BUFFER_SIZE);
    }
}
```

## 4. STM32H7B0 UAC2 DMA 双缓冲实现

### 4.1 UAC2 DMA 传输流程
```
USB UAC2 接收数据
    ↓
DMA 传输到内存缓冲区
    ↓
音频处理（DSP 等）
    ↓
DMA 传输到 I2S 外设
    ↓
PCM5102A DAC 播放
```

### 4.2 UAC2 DMA 双缓冲配置
```c
void configure_uac2_dma_double_buffer(void) {
    // 1. 配置 USB DMA 通道
    DMA_InitTypeDef dma_init;
    
    // 2. 配置 USB DMA 控制寄存器
    dma_init.DMA_Channel = DMA_CHANNEL_USB_OTG_FS;
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&USB_OTG_FS->FIFO(0);
    dma_init.DMA_Memory0BaseAddr = (uint32_t)usb_rx_buffer;
    dma_init.DMA_DIR = DMA_DIR_PeripheralToMemory;
    dma_init.DMA_BufferSize = UAC2_DMA_BUFFER_SIZE;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma_init.DMA_Mode = DMA_Mode_Circular;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_M2M = DMA_M2M_Disable;
    dma_init.DMA_DoubleBufferMode = DMA_DoubleBufferMode_Enable;
    
    // 3. 初始化 USB DMA 通道
    HAL_DMA_Init(&hdma_usb_rx);
    
    // 4. 配置双缓冲地址
    HAL_DMAEx_ConfigDoubleBufferMode(&hdma_usb_rx,
                                     (uint32_t)usb_rx_buffer1,
                                     (uint32_t)usb_rx_buffer2);
    
    // 5. 使能 DMA 通道
    __HAL_DMA_ENABLE(&hdma_usb_rx);
}
```

## 5. 性能分析

### 5.1 优点
1. **连续音频播放**：无缝切换缓冲区，避免音频卡顿
2. **高吞吐量**：DMA 传输速度快，减少 CPU 负载
3. **低延迟**：双缓冲设计减少了数据准备时间
4. **可扩展性**：易于扩展到多声道音频

### 5.2 缺点
1. **内存开销**：需要双倍的缓冲区内存
2. **复杂性**：DMA 配置和中断处理较复杂
3. **调试难度**：DMA 错误较难排查
4. **实时性要求**：需要精确的缓冲区管理

## 6. 调试技巧

### 6.1 DMA 状态监控
```c
void monitor_dma_status(void) {
    // 检查 DMA 状态寄存器
    uint32_t dma_isr = DMA1->ISR;
    uint32_t dma_ifcr = DMA1->IFCR;
    
    // 检查传输完成标志
    if (dma_isr & DMA_ISR_TCIF5) {
        // 传输完成
    }
    
    // 检查半传输标志
    if (dma_isr & DMA_ISR_HTIF5) {
        // 半传输完成
    }
    
    // 检查传输错误标志
    if (dma_isr & DMA_ISR_TEIF5) {
        // 传输错误
    }
}
```

### 6.2 缓冲区完整性检查
```c
void check_buffer_integrity(void) {
    // 检查缓冲区数据完整性
    for (int i = 0; i < DMA_BUFFER_SIZE; i++) {
        if (dma_buffer1[i] == 0xFFFFFFFF) {
            // 缓冲区损坏
            break;
        }
    }
    
    // 检查缓冲区同步
    if (current_buffer != dma_buffer1 && current_buffer != dma_buffer2) {
        // 缓冲区指针错误
    }
}
```

## 7. 最佳实践

### 7.1 DMA 配置
1. **优先级设置**：使用高优先级确保实时性
2. **突发传输**：根据外设特性设置适当的突发传输
3. **数据宽度**：匹配外设数据宽度
4. **地址增量**：根据传输方向设置地址增量

### 7.2 中断处理
1. **及时响应**：在中断服务例程中完成必要的处理
2. **避免阻塞**：不要在中断服务例例程中进行长时间操作
3. **状态标志**：正确清除 DMA 中断标志

### 7.3 缓冲区管理
1. **双缓冲设计**：确保缓冲区交替使用
2. **缓冲区填充**：在 DMA 传输完成时重新填充缓冲区
3. **缓冲区同步**：保持缓冲区指针的一致性
4. **错误处理**：处理缓冲区损坏的情况

## 8. STM32H7B0 DMA 双缓冲代码示例

```c
#include "stm32h7b0xx_hal.h"

// DMA 双缓冲配置
#define DMA_BUFFER_SIZE     (4096)
uint32_t dma_buffer1[DMA_BUFFER_SIZE];
uint32_t dma_buffer2[DMA_BUFFER DMA_BUFFER_SIZE];
uint32_t *current_buffer = dma_buffer1;

DMA_HandleTypeDef hdma_i2s_tx;

void SystemClock_Config(void);
static void MX_DMA_Init(void);
static void MX_I2S1_Init(void);
void fill_dma_buffer(uint32_t *buffer, uint32_t size);

int main(void) {
    HAL_Init();
    SystemClock_Config();
    
    MX_DMA_Init();
    MX_I2S1_Init();
    
    // 初始化缓冲区
    fill_dma_buffer(dma_buffer1, DMA_BUFFER_SIZE);
    fill_dma_buffer(dma_buffer2, DMA_BUFFER_SIZE);
    
    // 使能 DMA 通道
    __HAL_DMA_ENABLE(&hdma_i2s_tx);
    
    // 使能 I2S
    HAL_I2SEx_Transmit_DMA(&hi2s1, (uint16_t*)current_buffer, DMA_BUFFER_SIZE);
    
    while (1) {
        // 主循环处理
        HAL_Delay(1);
    }
}

void MX_DMA_Init(void) {
    hdma_i2s_tx.Instance = DMA1_Stream5;
    hdma_i2s_tx.Init.Channel = DMA_CHANNEL_I2S1_TX;
    hdma_i2s_tx.Init.Direction = DMA_MEMORY_TO_PERIPH;
    hdma_i2s_tx.Init.PeriphInc = DMA_PERIPH_INC_DISABLE;
    hdma_i2s_tx.Init.MemInc = DMA_MEM_INC_ENABLE;
    hdma_i2s_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_i2s_tx.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    hdma_i2s_tx.Init.Mode = DMA_NORMAL;
    hdma_i2s_tx.Init.Priority = DMA_PRIORITY_HIGH;
    hdma_i2s_tx.Init.FIFOMode = DMA_FIFOMODE_DISABLE;
    hdma_i2s_tx.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
    hdma_i2s_tx.Init.MemBurst = DMA_MBURST_SINGLE;
    hdma_i2s_tx.Init.PeriphBurst = DMA_PBURST_SINGLE;
    
    HAL_DMA_Init(&hdma_i2s_tx);
    
    HAL_DMAEx_ConfigDoubleBufferMode(&hdma_i2s_tx,
                                     (uint32_t)current_buffer,
                                     (uint32_t)current_buffer == (uint32_t)dma_buffer1 ? 
                                         (uint32_t)dma_buffer2 : (uint32_t)dma_buffer1);
}

void fill_dma_buffer(uint32_t *buffer, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = (i * 0x123456) & 0xFFFF;  // 生成测试数据
    }
}

void HAL_DMA_XferHalfCpltCallback(DMA_HandleTypeDef *hdma) {
    if (hdma->Instance == DMA1_Stream5) {
        current_buffer = (current_buffer == dma_buffer1) ? 
                         dma_buffer2 : dma_buffer1;
        
        __HAL_DMA_DISABLE(hdma);
        hdma->Instance->M0AR = (uint32_t)current_buffer;
        __HAL_DMA_ENABLE(hdma);
        
        fill_dma_buffer(current_buffer, DMA_BUFFER_SIZE);
    }
}

void HAL_DMA_XferCpltCallback(DMA_HandleType
```