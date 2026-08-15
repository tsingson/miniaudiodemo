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
排查方法：当时把整板原理图 PDF 用 `gs -sDEVICE=png16m` 渲出来看，确认走线（该 PDF 当前未纳入仓库）。
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
- **DSP 管道层**（`play_audio_pipeline.*` + `play_pipeline`/`play_pipeline_fanout`/`play_pipeline_mix`/`play_dsp_plugins`，均在 `src/dspeq/`）：新增 `play_pipeline_async.c/h`——一个通用的"队列+消费者线程"异步解耦适配器，队列深度、线程优先级和 scratch buffer 均由调用方静态提供，零动态分配。
- **输入输出适配（当前结构）**：`src/uac2/uac2_audio_stream.c` 统一拥有异步队列、工作线程、设备启动和统计快照；`main_pcm5102_st7735s_uac2.c` 只通过 `uac2_audio_stream_config` 提供 pipeline 处理回调和可选的 PCM5102A 缓冲占用查询。协议层 `src/uac2/usb_uac2_device.c` 不认识 pipeline 或 PCM5102A，应用入口也不再直接调用底层 `usb_uac2_*` API。

**方法论**：解耦不是把回调函数签名做成 `void*` 就完了，关键是"谁拥有缓冲/时序控制权"。当前由 `uac2_audio_stream` 拥有 UAC2 到 pipeline 的异步交接，通用队列实现仍由 `play_pipeline_async` 提供；协议层和 PCM5102A 输出层都不感知彼此。判断一个信号是否适合作为耦合点，还要看时间尺度：反馈信号需要能反映"秒级"趋势，用只有约 8ms 深的异步交接队列作为反馈来源，天生采不到趋势（见 3.7）。

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

### 3.9 其他已确认无问题的点

- **UAC2 采样率协商**：`get_sample_rate`/`set_sample_rate` 回调故意留 `NULL`——因为 devicetree 的 Clock Source 只声明了一个采样率（`sampling-frequencies = <48000>`），按 `usbd_uac2.h` 文档说明这种情况下这两个回调本来就应该是 NULL，不是遗漏，也不需要额外实现协商逻辑。
- **速率匹配协商**：默认构建走显式反馈端点（`feedback_cb`），时钟源没设 `sof-synchronized`，配置是自洽的（但见 3.12，"配置自洽"不等于"主机真的会持续使用它"）。

### 3.10 内存重新分配踩了一次死锁坑：`PCM_PRIME_BLOCKS` 不能超过驱动自己的队列深度

去掉屏幕阻塞之后，`audio_drop=0` 但残余 ~2s 周期性卡顿依旧存在。为了在不增加总 RAM 的前提下换取更深的 I2S 发送缓冲，做了一次内存重排：

- `pcm5102a_audio.c` 的 `PCM_BLOCK_FRAMES` 从 512 降到 **128**（正好是 UAC2/`play_mcu`/`main_pcm5102_st7735s` 这些实时调用方本来就在喂的粒度，不需要再多攒一层批）。
- 这样 `g_output_accumulator` 从 4096 字节缩到 1024 字节（4 倍），省出的 3072 字节全部投入 `PCM_SLAB_BLOCK_COUNT`（4 → 20 块）。
- `pcm5102a_audio_output_stage()` 改成能安全处理"任意帧数"的分块循环，兼容仍然一次喂 512 帧的 `main_pcm5102a_test.c`。
- 结果：I2S 发送缓冲从 42.7ms 提升到 53.3ms，总 RAM 占用反而**净减少 1024 字节**（62484B→61460B）。

但这一步引入了一个新 bug——**完全没有声音**：`PCM_PRIME_BLOCKS` 为了保持跟旧版本相近的预热时长，从 4 改成了 16，但忘了预热阶段的 `i2s_write()` 调用发生在 `I2S_TRIGGER_START` **之前**，这时候 DMA 还没启动，没有任何东西在消费 Zephyr `i2s_stm32` 驱动自己内部的发送队列（深度由 `CONFIG_I2S_STM32_TX_BLOCK_COUNT=12` 决定，见 `prj.conf`）。预热目标 16 > 队列深度 12，从第 13 次预热写入开始，`i2s_write()` 永远排不进去，每次都等满 1000ms 配置超时后返回 `-11`（EAGAIN）——**播放永远无法真正启动**，且故障恰好每 1 秒出现一次（正好是超时时长），`out_delivered` 几乎不增长。

修复：`PCM_PRIME_BLOCKS` 改成 **8**（必须严格小于驱动自己的 12）。

**教训**：任何"预热/priming"逻辑，只要是在触发底层外设真正启动**之前**调用写入接口，就必须确认底层驱动自己的排队深度（不是我们自己的 `mem_slab`），预热目标不能超过它，否则会形成"必须先攒够 N 个才能启动，但队列容不下 N 个"的死锁，且这类死锁往往表现得像是普通的超时性错误而非死锁，容易漏判。

### 3.11 加了直接可观测的 slab 占用诊断，证实残余卡顿确实是真实的时钟漂移

之前两次自适应反馈尝试都让 `fb_adj` 卡在一个很小的常数上，一直没有直接证据判断残余卡顿到底是不是时钟漂移。这次加了 `pcm5102a_audio_sample_slab_range()`，记录每次状态打印间隔内 I2S 发送 slab 占用的 min/max（读一次清零一次）。

上机数据显示一个非常干净的信号：每次 `-EIO` 重启后，`slab_max` 会回到接近满值（如 9，容量 20 块的一半左右），然后**逐个采样周期单调下降**（9→8→7→6...）直到下一次故障重启，重启后又跳回高值——这是"持续性平均速率失配"的典型信号，不是偶发抖动。按缓冲时长（53.3ms）除以故障周期（~1.9-2.1s）估算，真实漂移量级约 2.5-3%，和早前的估算吻合。**这次终于有直接证据，而不是靠猜。**

### 3.12 反馈机制本身其实没用上：主机在协商之后就不再轮询了

既然确认是真实漂移，理应靠 UAC2 显式反馈端点纠正。于是又加了两个诊断：`usb_uac2_get_feedback_call_count()`（`feedback_cb` 被调用的总次数）和 `usb_uac2_sample_feedback_fill_range()`（`feedback_cb` 自己看到的占用率范围）。

结果非常明确：**`fb_calls` 从头到尾停留在 4**，此后再也没有变化过；`fb_fill_min`/`fb_fill_max` 长期保持在重置的初始值（min > max），说明从上次重置到现在压根没采到新样本。也就是说：**macOS 的 CoreAudio 驱动只在设备枚举/协商阶段调用过我们的反馈端点 4 次，之后就再也不来读取更新后的反馈值了**。

这解释了此前三轮反馈调参（缓冲区来源、目标千分比、增益、修正范围）为什么全都没有实质效果——不是控制律没调对，是主机压根不再读这个值，无论设备侧算得多准都是徒劳。当前 devicetree 声明的 Clock Source 只有一个固定采样率（`sampling-frequencies = <48000>`），推测 macOS 认为这种"单一定频"设备不需要持续的显式反馈校正，只在建连时采样一次就切换到自己内部的时钟恢复机制。

**这是当前配置（显式反馈，explicit feedback）在这套 macOS+STM32 组合下的一个实际限制，不是我们控制律实现的问题。**

### 3.13 隐式反馈实验 + "换用自己的 macOS 发送端能不能帮上忙"的分析

加了一个可切换的隐式反馈构建变体（`./r.sh build-uac2-implicit`/`flash-uac2-implicit`），复用已有的 `boards/stm32f401rct6_uac2_implicit.overlay`（之前只在 null-sink 协议测试里用过）接上真实 PCM5102A 输出，跟现有的显式反馈版本（`build-uac2-stm32`）互为可切换的对照组。注意：Zephyr `zephyr,uac2-audio-streaming` 绑定文档里 `implicit-feedback` 对 OUT 流的效果是**直接去掉显式反馈端点**，并不提供替代的校正机制——所以这更像是"看 macOS 在没有反馈端点时内部时钟恢复策略是否不同"的对照实验，不是一个确定有效的修复方案，需要实测。

同时讨论了另一个问题："如果用 `src/main_uac2_srv.c`（我们自己写的 macOS 发送端）代替系统自带播放器，双方配合能不能做到更流畅？"

结论：**光换发送端本身没有用**。不管是系统播放器还是 `main_uac2_srv.c`，在 macOS 上都要经过同一条路径：

```
(系统播放器 或 main_uac2_srv.c 的 miniaudio ma_device)
        -> 都走 CoreAudio API
        -> macOS 系统自带的 AppleUSBAudio 驱动（黑盒，应用层完全无法控制）
        -> UAC2 等时传输 + 反馈端点轮询策略
        -> STM32
```

`main_uac2_srv.c` 只是换了"喂给 CoreAudio 什么内容、怎么处理"，并没有绕开 CoreAudio 和苹果自带的 USB Audio 驱动——而 3.12 已经证实，卡顿根因正是这个黑盒驱动在协商后不再轮询我们的反馈端点，这是传输层行为，跟"用什么播放器/发送端"无关。

但确实有两个能真正利用"发送端代码也是我们自己的"这个优势的方向，记录下来作为以后的研究方向：

- **方向 A：静态速率补偿**。已经用 `slab_min`/`slab_max` 数据估算出真实漂移约 2.5-3%，且方向稳定（缓冲池持续变空）。可以在 `main_uac2_srv.c` 里给发送内容加一个固定的重采样/变速系数（比如让播放的相位步进比正常慢 2.5-3%，方向需要现场试出来），补偿 STM32 那颗板子偏快的真实 I2S 时钟。改动小、见效快，但只是针对*这块板子*当前 HSI 状态的静态标定，换板子或温度大幅变化需要重新调，而且系统自带播放器享受不到这个补偿（只有走 `main_uac2_srv.c` 才有效）。
- **方向 B：应用层旁路动态反馈**。既然 UAC2 反馈端点被系统忽略，可以绕开它，复用板子已有的 USART1 串口另开一条我们自己完全控制的通道，让 STM32 把自己的 I2S 缓冲占用实时报回来，`main_uac2_srv.c` 收到后动态调整自己的重采样速率——相当于在应用层自己实现一遍 UAC2 反馈端点本该做但系统不做的事。真正能自适应，但需要新的串口协议、STM32 侧发送状态帧、Mac 侧动态重采样器，工作量明显大于方向 A。

两个方向都还没有实现，留作后续研究方向（建议先做方向 A 验证漂移方向/幅度估算是否准确，效果不够或想要真正自适应再上方向 B）。

## 4. 当前状态（截至本次更新）

- USB 枚举、buffer 泄漏、栈溢出、阻塞式 I2S 写入、`audio_drop` 持续增长、屏幕阻塞主线程、预热死锁 —— 均已修复并上机验证，`audio_drop` 现在稳定为 0。
- I2S 发送缓冲已从 42.7ms 提升到 53.3ms，同时净省了 1KB RAM。
- 仍有残余的周期性小卡顿（~1.9-2.1 秒一次 `-EIO`），现已直接证实是真实存在的 HSI/PLLI2S 时钟漂移（约 2.5-3%），且**当前基于显式反馈端点的自适应校正在实践中不生效**（主机不持续轮询）。
- 新增可切换的隐式反馈构建变体（`build-uac2-implicit`）；初步主观听感有改善，但仍需用相同音源和长时间串口计数做量化对照。
- ST7735S 显示已从 UAC2 接收端整个移除。
- 用户反馈：当前实现听感已有改进（从"持续丢包+频繁卡顿"到"链路健康+偶发周期性小卡顿"），但还不够理想。

## 5. 待办 / 后续方向

1. **反馈机制**：显式反馈（explicit feedback）路线已确认在当前 macOS 行为下走不通。可选方向：
        - 隐式反馈对照实验（`build-uac2-implicit`）：已有初步听感改善，仍需量化验证。
        - **`main_uac2_srv.c` 静态速率补偿**（方向 A，见 3.13）：改动小、见效快，建议优先尝试。
        - **`main_uac2_srv.c` + USART1 应用层旁路动态反馈**（方向 B，见 3.13）：真正自适应的长期方案，工作量较大。
   - 或深入校准 HSI/PLLI2S 时钟源本身（板级时钟精度调优，难度和风险都更高）。
   - 或接受当前状态，继续加大缓冲区换取更长的卡顿间隔（纯软件、低风险，但治标不治本）。
2. RAM 仍然紧张（UAC2 版本 ~93.78% 用量），继续加大缓冲需要先从别处继续省内存。
3. `main_uac2_srv.c`（macOS 发送端）和 `play_macos.c` 高度重复（WAV 读取、planar 转换、DSP 调用、HTTP 控制线程全部复制粘贴），且都没有走 `play_pipeline`/`play_audio_pipeline` 抽象，而是直接调 `play_dsp_process_planar`——待后续整理，让 macOS 侧也复用同一套管道组装代码。


