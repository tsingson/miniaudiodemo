# macOS → UAC2 → STM32F401 → PCM5102A 开发日志

记录 UAC2 音频接收端从"能编译"到"真机能稳定播放"过程中的关键节点、根因排查方法和经验教训。旧版本日志描述的是硬件验证之前的脚手架阶段，内容已过时，本次全部重写。

## 1. 目标架构

```text
macOS (CoreAudio/miniaudio 选择 USB Audio 播放设备)
        |  UAC2 等时 OUT 传输
        v
STM32F401RCT6  (Zephyr device_next UAC2 class, USBD_SPEED_FS)
        |  PCM16 立体声 48kHz
        v
I2S2 + PLLI2S(HSI) + DMA
        |  DIN/BCK/LRCK（无 MCLK）
        v
PCM5102A -> LINE OUT -> 功放
```

- macOS 是 USB Host，STM32F401 是 UAC2 Device，用系统自带的 CoreAudio 驱动枚举播放设备，不需要 libusb 之类的用户态注入。
- STM32 侧使用 Zephyr 较新的 `device_next`/UDC 栈（`CONFIG_USB_DEVICE_STACK_NEXT`），而不是老的 `CONFIG_USB_DEVICE_STACK`。
- 板子是无晶振方案：HSI(16MHz) → 系统 PLL(84MHz) 和 PLLI2S 分别产生系统时钟和 I2S 位时钟，USB 走 PLLQ。

## 2. 当前构建/烧录命令

STM32 接收端：

```sh
./r.sh build-uac2-stm32      # 调试版，串口 USART1 (PA9/PA10) 输出日志
./r.sh flash-uac2-stm32
./r.sh build-uac2-null       # 只测协议层，不接 PCM5102A（协议压力测试用）
./r.sh build-uac2-stm32-prod # 生产配置
```

macOS 发送端（把本地 WAV + 共用 DSP 处理后的音频，通过 UAC2 发给板子）：

```sh
./r.sh build-uac2-macos
UAC2_DEVICE_NAME="STM32F401" ./r.sh run-uac2-macos   # 设备名填不对会打印所有可选设备名
```

真机验证需要**三个独立的物理连接同时存在**（最容易漏第三个）：
1. SWD 调试器 —— 烧录用
2. USART1 ↔ USB-TTL 适配器 —— 串口日志（`uac2` 相关 overlay 把 console 从 USB CDC 改到了 usart1）
3. 板子自己的 Type-C 口 ↔ Mac —— 真正的 UAC2 音频数据链路

## 3. 关键 Bug 时间线（按发现顺序）

### 3.1 USB 无法枚举

现象：烧录成功，串口日志正常，但 macOS 完全看不到设备。
根因：一直以为 PA11/PA12 需要额外接线，实际上板子（Jessinie STM32F401RCT6 Type-C 核心板）的 Type-C 口已经直接接了 PA11(D-)/PA12(D+) 和 5.1kΩ CC 下拉电阻——**只是没插板子自己的 Type-C 口到 Mac**（只接了调试器和串口）。
排查方法：把 `docs/stm32f401rct6_chip.pdf`（实际是整板原理图，只有一页）用 `gs -sDEVICE=png16m` 渲出来看，确认走线。
教训：三个连接分别独立，缺一个的现象是"其他一切正常，就是不出声/不枚举"，容易误判为软件问题。

### 3.2 接收缓冲区内存泄漏

现象：`UAC2 RX packets` 计数在恰好等于 `uac2_rx_slab` 配置的块数（试过 8 和 32）时冻结不再增长，但 `TERM ON`、buffer 请求数还在涨——说明 USB 协议层完全正常，是应用层的内存管理问题。
根因：`uac2_data_recv_cb()` 只在"奇数字节数错误"分支里调用了 `k_mem_slab_free()`，正常成功路径和 0 字节路径都没有释放缓冲区。
排查方法：读 Zephyr `subsys/usb/device_next/class/usbd_uac2.c` 源码确认：**OUT 端点的 class 层只调 `data_recv_cb`（所有权转移给应用），从不调 `buf_release_cb`**（那个只用于 IN/feedback）。加了 `usb_uac2_get_release_count()` 计数器验证 `buf_release_cb` 调用次数一直是 0，证实泄漏点。
修复：在 `uac2_data_recv_cb()` 成功路径末尾补上 `k_mem_slab_free(&uac2_rx_slab, buf)`。
**方法论**：Zephyr 某个 class 的 ops 结构体如果同时有"recv"和"release"两个回调，不要假设对称，一定要去读 class 源码确认 OUT/IN 各自谁负责释放。

### 3.3 栈溢出（最隐蔽的一个）

现象：`pcm5102a_audio_init()` 返回 0 之后，程序静默"消失"，没有任何崩溃日志。
根因：`main()` 里 `play_audio_pipeline pipeline;` 是**栈上局部变量**，而这个结构体内嵌了完整的 `play_dsp_state`（16 波段 EQ 系数/状态 + 4 个 512-float 的 analyzer/observer 缓冲区）+ `planarStorage[2][512]`，实测约 **18.6KB**（`arm-zephyr-eabi-nm --print-size --size-sort` 确认符号大小 `0x48c0`），而 `CONFIG_MAIN_STACK_SIZE` 只有 4096 字节，且没配 MPU 栈保护，栈溢出直接静默踩坏相邻内存，不会崩溃出日志。
排查方法：正常 `LOG_INF` 当时不可靠，改用手动在 `main()`/`pcm5102a_audio_init()` 每一步后插入 `st7735s_log_display_line()` 面包屑，定位到"死"在两行面包屑之间。
修复（三处配合，缺一不可）：
1. `pipeline` 改成 `static play_audio_pipeline pipeline;`（挪到 `.bss`，让链接器在编译期就检查预算，而不是运行时静默踩坏——这一步单独做完之后链接直接报"RAM 超预算 8980 字节"，这才坐实了真实大小）。
2. `play_dsp_defs.h` 里的 `ANALYZER_WINDOW` 加 `#ifndef` 让它可以按目标覆盖，Zephyr MCU 目标定义为 32（这些 analyzer/observer tap 缓冲区在嵌入式端是没人用的遥测数据，默认 512 太大）。
3. `prj.conf`：`CONFIG_MAIN_STACK_SIZE` 4096→2048，`CONFIG_HEAP_MEM_POOL_SIZE` 16384→8192，腾出余量。
**附带收获**：这个改动顺带修好了一个跟本次会话无关的既有 bug——默认的 `./r.sh build`（`play_mcu.c`）在这次改动之前就一直因为同样的根因（analyzer 缓冲区太大）而 RAM 超限链接失败（用 `git stash` 做过 A/B 验证）。
**方法论**：任何嵌入式 target 的 `main()` 里，凡是把内嵌 `play_dsp_state` 的结构体声明成非 `static` 局部变量，都是栈溢出高危项，先 `grep "play_audio_pipeline \w+;"`（非 static）过一遍再排查别的。

### 3.4 阻塞式 I2S 写入卡住 USB 接收线程

现象：`i2s_write()` 在 TX slab 耗尽时用 `K_FOREVER` 等待，而这个调用链是从 USB 接收回调（`uac2_data_recv_cb` → `pipeline_sink` → ... → `pcm5102a_audio_write_float`）**同步**发起的，一旦阻塞就直接错过下一个等时帧。
修复：`write_float_unlocked()` 改成 `K_MSEC(2)` 有界超时，超时就丢弃当前块而不是无限等待。
后续发现（见 3.7）：这个方向是对的，但当时把它和"USB 线程被谁"混在一起判断，走了弯路。

### 3.5 架构重构：uac2 / dsp pipeline / 输出解耦

用户要求把代码整理成三个清晰解耦的部分。改动前 `usb_uac2_device.c` 里直接 `#include "pcm5102a_audio.h"`，`feedback_cb` 直接读 PCM5102A 的 I2S 发送缓冲区占用——这是一处硬耦合。

重构方案：
- **UAC2 输入层**（`usb_uac2_device.c/h`）：只管 USB host/device 交互和数据接收，不认识任何具体输出设备。对外只暴露两个通用回调注册接口：`usb_uac2_set_audio_sink()`（推送音频）和 `usb_uac2_set_feedback_source()`（查询下游缓冲占用率，返回 0-1000 千分比）。
- **DSP 管道层**（`play_audio_pipeline.*` + `play_pipeline`/`play_pipeline_fanout`/`play_pipeline_mix`/`play_dsp_plugins`，均在 `src/dspeq/` 或 `src/`）：新增 `play_pipeline_async.c/h`——一个通用的"队列+消费者线程"异步解耦适配器，把原来写死在 UAC2 文件里的缓冲/线程搬到这里，做成可复用组件（队列深度、线程优先级、scratch buffer 全部由调用方静态提供，零动态分配）。管道层从此统一拥有"时序解耦 + 缓存"的职责。
- **输入输出适配**：`main_pcm5102_st7735s_uac2.c` 作为组合根（composition root），负责把 UAC2 输入、异步解耦适配器、PCM5102A 输出这三者接起来——`usb_uac2_set_audio_sink(play_pipeline_async_push, &audio_out_async)`，`usb_uac2_set_feedback_source(pcm5102a_fill_permille, NULL)`。只有组合根知道所有模块的存在，模块之间互相不知道。

**方法论**：解耦不是把回调函数签名做成 `void*` 就完了，关键是"谁拥有缓冲/时序控制权"——这部分逻辑应该长在管道层，而不是长在输入模块或者输出模块里。判断一个信号是否适合作为耦合点，要看它的时间尺度：反馈信号需要能反映"秒级"的趋势，用一个只有 8ms 深的队列去做这个信号来源，天生就采不到趋势（见 3.7）。

### 3.6 "DMA burst size error" 是无关紧要的误导项

每次 I2S 恢复都会打印 `Memory/Peripheral burst size error, using single burst as default`，一度怀疑是这个导致周期性丢帧。
排查：读 Zephyr `drivers/i2s/i2s_stm32.c` 的 `I2S_DMA_CHANNEL_INIT` 宏，发现 `source_burst_length`/`dest_burst_length` **写死为 2**，而 `drivers/dma/dma_stm32_v1.c` 的 `stm32_dma_get_mburst/get_pburst` 只接受 `{1,4,8,16}`，2 永远落入 `default` 分支——**这是 Zephyr 驱动本身在所有配置下都会触发的固定行为，从开机第一次 configure 就会出现，只是单纯 fallback 成单次传输模式，功能上完全正常，跟我们自己的周期性故障没有因果关系**。
教训：日志里"看起来吓人"的 ERR 级别信息，要先确认它是不是每次都出现（包括正常工作的时候），而不是想当然地当成故障根因。

### 3.7 自适应反馈：两次尝试都没有实质效果，及原因

第一次实现：反馈信号取自新建的 `play_pipeline_async` 队列占用率（队列深度 3，只有 ~8ms）。`fb_adj` 很快钉死在很小的值（±4），完全不随后续持续的 `-EIO` 故障继续变化。
分析：这个队列太浅，占用情况被调度抖动主导，根本反映不出秒级的平均速率漂移趋势。真正吸收漂移的是 PCM5102A 自己的 I2S 发送 mem_slab（4 块 = 42.7ms）。
第二次实现：改成查询 PCM5102A 的真实 I2S 发送缓冲占用率（保持架构解耦——通过组合根里的适配函数接线，UAC2 模块本身仍然不认识 pcm5102a），反馈修正范围从 ±0.5% 放宽到 ±5%，步长从固定改成按误差比例。
结果：`fb_adj` 提高到了 32，仍然很快钉死不再变化，`-EIO` 周期毫无变化。
**这两次尝试都指向同一个错误方向**：真正的根因根本不是"缓冲区被平均速率漂移缓慢耗尽"，而是 3.8 描述的"周期性整体冻结"。用一个瞬时占用率去拟合一个不存在的"缓慢漂移"，自然怎么调都没用。

### 3.8 真正的根因：屏幕刷新把主线程堵死了（本次会话最大的发现）

现象：`-EIO` 恢复周期稳定在 ~1.4-1.46 秒，规律得不像是内容相关的抖动，更像是某个固定周期的事件。同时发现串口状态行本该每 500ms 打印一次，实际却是 ~1.4-1.46 秒才打印一次——**这个时间差本身就是线索，但一开始被忽略了**。

根因定位：`display_uac2_status()` 每 500ms 调一次，内部调 7 次 `st7735s_log_display_line()`；而 `st7735s_log_display_stub.c` 的实现是**每个点亮的像素单独发一次阻塞式 SPI `display_write()`**（`draw_glyph()` 对 5×7 字形里每个置位的 bit 都单独调 `fill_rect(...,1,1,...)`）。一行 ~15 个字符，7 行状态加上背景填充，一次刷新大约要发出**上千次独立的阻塞 SPI 传输**，估算耗时接近 1 秒。

而 `main()` 跑在 Zephyr 默认线程优先级（数值 0，抢占式线程里优先级最高），高于 USB 协议栈线程（`CONFIG_UDC_STM32_THREAD_PRIORITY=8`）和音频消费线程 `audio_out_tid`（10）。**每次刷屏，主线程会连续抢占 CPU 将近 1 秒，这段时间里 USB 线程和音频输出线程完全无法运行**：I2S TX slab 得不到补充 → DMA 断供 → `-EIO`；USB 等时传输窗口也被错过 → `audio_drop` 持续增长。500ms 刷新间隔 + ~900ms 阻塞刷屏 ≈ 正好对上观察到的 ~1.4-1.46 秒周期。

这也解释了 3.7 里为什么反馈控制器怎么调都没用：缓冲区不是被"慢慢耗尽"的，而是整个系统在阻塞期间被冻结，反馈信号采样不到真实趋势，只能在冻结前后的短暂正常窗口里振荡，最终稳定在一个很小的、没有实际意义的值上。

修复（分两步）：
1. **临时规避**（先验证假设）：`main_pcm5102_st7735s_uac2.c` 里播放期间（`usb_uac2_stream_active()` 为真）跳过屏幕刷新，只保留串口 `LOG_INF`。上机验证：`audio_drop` 从持续增长变成完全不再增长，`-EIO` 周期从 ~1.45s 拉长到 ~2.0-2.1s——**验证了假设，效果立竿见影**。
2. **彻底根除**：既然串口日志已经够用，直接把 ST7735S 显示从 UAC2 接收端整个移除（不再 `#include`、不再初始化、CMakeLists 里对 `MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN` 这个 target 不再编译 `st7735s_log_display_stub.c`），彻底消除风险，同时省下约 2.3KB Flash。

**方法论（本次会话最重要的一条）**：
- 排查"周期性"故障时，**周期本身的数值是最重要的线索**——如果发现另一个"本该是固定周期"的东西（比如日志打印间隔）实际周期跟故障周期高度吻合但又不等于设计值，几乎可以确定两者同源。
- 任何在**高优先级线程**（包括默认优先级的 `main()`）里做的、没有让出 CPU 的**同步阻塞 I/O**（尤其是逐字节/逐像素发起多次很小的外设事务），都可能无差别地饿死其他所有低优先级线程，哪怕这些线程之间看起来毫不相关（USB 栈和音频输出线程跟屏幕显示本来八竿子打不着）。
- 调试外设（屏幕、LED 之类）不应该和关键实时路径共享同一个高优先级上下文；要么降低调试输出频率，要么把它放到独立的低优先级线程/workqueue，要么像这次一样在关键路径活跃时直接跳过。

## 4. 其他已确认无问题的点

- **UAC2 采样率协商**：`get_sample_rate`/`set_sample_rate` 回调故意留 `NULL`——因为 devicetree 的 Clock Source 只声明了一个采样率（`sampling-frequencies = <48000>`），按 `usbd_uac2.h` 文档说明这种情况下这两个回调本来就应该是 NULL，不是遗漏，也不需要额外实现协商逻辑。
- **速率匹配协商**：走的是显式反馈端点（`feedback_cb`），时钟源没设 `sof-synchronized`，配置是自洽的。

## 5. 当前状态（截至本次重写）

- USB 枚举、buffer 泄漏、栈溢出、阻塞式 I2S 写入、`audio_drop` 持续增长、屏幕阻塞主线程 —— 均已修复并上机验证。
- `audio_drop` 计数器已完全停止增长，链路本身健康。
- 仍有残余的周期性小卡顿（~2.0-2.1 秒一次 `-EIO`），但已经从"持续丢包"改善为"偶发短暂卡顿"，用户反馈主观流畅度有明显提升但还不够好。
- 已把 ST7735S 显示从 UAC2 接收端整个移除，进一步排除该风险，继续优化中。

## 6. 待办 / 后续方向

1. 残余的 ~2s 周期性卡顿：需要重新评估——之前基于"平均速率漂移"的自适应反馈假设已经在 3.7 中被证明作用甚微，去掉屏幕阻塞这个大变量之后，应该重新采集数据判断这是真实的时钟漂移，还是还有另一个尚未发现的周期性阻塞源。
2. RAM 已经很紧张（UAC2 版本 ~95% 用量），如果要加大 PCM5102A 的 I2S 发送缓冲深度（当前只有 4 块 = 42.7ms）来换取更多容错余量，需要先从别处省出内存。
3. `main_uac2_srv.c`（macOS 发送端）和 `play_macos.c` 高度重复（WAV 读取、planar 转换、DSP 调用、HTTP 控制线程全部复制粘贴），且都没有走 `play_pipeline`/`play_audio_pipeline` 抽象，而是直接调 `play_dsp_process_planar`——待后续整理，让 macOS 侧也复用同一套管道组装代码。
