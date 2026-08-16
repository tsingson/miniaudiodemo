# STM32H7B0VBT6 FK7B0M1 开发日志

## 1. 当前目标

目标板为 FK7B0M1 STM32H7B0VBT6 核心板，目标功能与 F401 接收端一致：

```text
macOS USB Host
    -> USB Audio Class 2 Full-Speed OUT
    -> STM32H7B0VBT6
    -> I2S2 + DMA
    -> PCM5102A
```

当前代码入口：

- `src/main_pcm5102_stm32h7b0_uac2.c`: H7B0 板级薄入口
- `src/uac2/uac2_pcm5102_app.c`: F401/H7B0 共用 UAC2 + PCM5102A 应用
- `src/uac2/uac2_audio_stream.c`: UAC2 异步队列、工作线程、设备启动和统计
- `src/uac2/usb_uac2_device.c`: UAC2 描述符、端点和 PCM 数据接收
- `src/pcm5102/pcm5102a_audio.c`: I2S2/PCM5102A/DMA 输出
- `boards/stm32h7b0vbt6_uac2.overlay`: FK7B0M1 板级设备树配置

## 2. FK7B0M1 的 USB OTG 在哪里

### 芯片 USB OTG 节点

Zephyr 使用：

```dts
&usbotg_hs {
    pinctrl-0 = <&usb_otg_hs_dm_pa11 &usb_otg_hs_dp_pa12>;
    pinctrl-names = "default";
    status = "okay";
};
```

这里的 `usbotg_hs` 是 STM32H7B0 的 OTG HS USB 控制器。当前配置把它作为 **Full-Speed USB Device** 使用，UAC2 是设备端，macOS 是 Host。

实际 USB 信号：

- `PA11`: `USB_D-`
- `PA12`: `USB_D+`
- `USB_VBUS`: USB 5V 检测/供电网络，按板级 USB 电路连接
- `USB_ID`: OTG 主从角色相关信号，当前 UAC2 Device 模式不依赖主机模式

### FK7B0M1 物理连接位置

核心板资料：

```text
docs/STM32H7B0VBT6核心板（型号FK7B0M1）/
```

原理图中可以看到：

1. STM32H7B0 芯片左侧明确标出 `PA11 USB_D-` 和 `PA12 USB_D+`。
2. 核心板的 USB 差分网络命名为 `USB_D-` / `USB_D+`。
3. Type-C USB1 接口电路位于配套 miniIF/扩展接口电路中，通过 `USB_D-` / `USB_D+` 连接到核心板 MCU。
4. 因此，USB 接口不应只看核心模块丝印；需要使用带 USB Type-C 接口的配套底板/miniIF 电路，或者从核心板引出的 `PA11/PA12`、VBUS、GND、CC 电路自行连接 USB Device 插座。
5. Type-C 插座的两个 CC 端需要按原理图保留 Device 端下拉电阻，否则 macOS 可能不会把它识别成 USB Device。

结论：

```text
USB OTG 控制器：STM32H7B0 内部 usbotg_hs
USB D-       ：PA11
USB D+       ：PA12
物理 USB 口  ：FK7B0M1 配套 miniIF/扩展板上的 USB1 Type-C
```

如果当前只拿着 FK7B0M1 核心模块，没有带 USB1 的扩展板，那么 `/dev/cu.usbmodem...` 不会因为烧录成功而自动出现，需要把核心板的 USB 差分和 Type-C Device 电路接出来。

## 3. H7B0 PCM5102A 引脚

当前 overlay 使用 I2S2：

```text
PB13 -> I2S2_CK -> PCM5102A BCK
PB12 -> I2S2_WS -> PCM5102A LRCK
PB15 -> SPI2_MOSI / I2S2_SD -> PCM5102A DIN
```

不使用 MCLK/SCK。当前代码不配置 PCM5102A 的 FLT、DEMP、XSMT、FMT GPIO，这些引脚按模块硬件电平连接。

USART1 日志：

```text
PA9  -> USART1_TX
PA10 -> USART1_RX
115200 baud
```

## 4. H7B0 设备树和时钟

`boards/stm32h7b0vbt6_uac2.overlay` 已启用：

- `uac2_audio` playback-only Audio Function
- 固定 48kHz、双声道、PCM16
- `usbotg_hs` Full-Speed USB Device
- `i2s2`
- `dma1`
- `dma2`
- `dmamux1`
- USART1 console

FK7B0M1 使用 25MHz HSE。H7B0 的 I2S 时钟资源比 F401 更丰富，SoC DTS 为 I2S2 提供 PLL1_Q 音频时钟选择。后续如果需要进一步降低时钟漂移，应优先检查 PLL1_Q/I2S 分频配置，而不是继续沿用 F401 的 HSI/PLLI2S 假设。

## 5. 构建与烧录

构建：

```sh
./r.sh build-uac2-h7b0
```

当前构建产物：

```text
build-zephyr-h7b0vbt6-uac2/zephyr/zephyr.elf
build-zephyr-h7b0vbt6-uac2/zephyr/zephyr.hex
```

H7B0 官方 board 定义的 Flash 区域为 128KB，因此 H7B0 使用 lite 模式：

- 保留 UAC2 PCM16/48kHz 接收
- 保留异步音频队列
- 保留 PCM5102A 输出
- 不编译本地 WAV fallback
- 不编译完整 DSP/plugin 链

已验证资源：

```text
FLASH: 88608 B / 128 KB = 67.60%
RAM:   51988 B / 256 KB = 19.83%
```

## 6. WCH-DAPLink + Homebrew OpenOCD

H7B0 使用 WCH-DAPLink，不是 ST-Link。使用 Homebrew OpenOCD：

```sh
command -v openocd
# /opt/homebrew/bin/openocd
```

OpenOCD 配置：

```text
interface/cmsis-dap.cfg
target/stm32h7x.cfg
```

探测/调试：

```sh
OPENOCD=/opt/homebrew/bin/openocd
SCRIPTS=/opt/homebrew/share/openocd/scripts

$OPENOCD \
  -s "$SCRIPTS" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32h7x.cfg \
  -c "adapter speed 1000; init; reset halt; shutdown"
```

实际探测结果：

```text
CMSIS-DAPv2 VID:PID = 0x1a86:0x8012
Serial              = A4A88F062A31
SWD DPIDR           = 0x6ba02477
Target              = Cortex-M7 r1p1
```

直接烧录并校验：

```sh
$OPENOCD \
  -s "$SCRIPTS" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32h7x.cfg \
  -c "adapter speed 1000; init; reset halt; program build-zephyr-h7b0vbt6-uac2/zephyr/zephyr.hex verify; reset run; shutdown"
```

已经验证：

```text
Device: STM32H7Ax/7Bx
Flash size: 128k
Programming Finished
Verified OK
```

### 调试连接

WCH-DAPLink 持久 GDB 服务：

```sh
$OPENOCD \
  -s "$SCRIPTS" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32h7x.cfg \
  -c "adapter speed 1000; gdb_port 3333; tcl_port disabled; telnet_port disabled"
```

如果运行中的 H7B0 让 OpenOCD 报：

```text
Cortex-M PARTNO 0x0 is unrecognized
```

使用硬件复位保持连接：

```sh
$OPENOCD \
  -s "$SCRIPTS" \
  -f interface/cmsis-dap.cfg \
  -f target/stm32h7x.cfg \
  -c "reset_config srst_only srst_nogate connect_assert_srst; adapter speed 500; init; reset halt; targets; shutdown"
```

该方式已成功识别：

```text
Cortex-M7 r1p1 processor detected
8 breakpoints, 4 watchpoints
```

## 7. 串口日志

当前已知 H7B0 串口设备：

```text
/dev/cu.usbmodemA4A88F062A311
```

查看日志：

```sh
SERIAL_PORT=/dev/cu.usbmodemA4A88F062A311 ./r.sh monitor
```

注意：`r.sh monitor` 使用 `screen`，退出方式为：

```text
Ctrl-A，然后 K
```

## 8. USB 测试顺序

1. 确认 FK7B0M1 已插入带 USB1 Type-C 的 miniIF/扩展板，或已自行接出 PA11/PA12 与 Type-C Device 电路。
2. 确认 USB Type-C 线连接到 Mac；WCH-DAPLink 只负责 SWD，不是 UAC2 音频线。
3. 烧录：

   ```sh
   ./r.sh build-uac2-h7b0
   ./r.sh flash-uac2-h7b0
   ```

4. 观察 USART1：

   ```sh
   SERIAL_PORT=/dev/cu.usbmodemA4A88F062A311 ./r.sh monitor
   ```

5. 在 Mac 检查 USB 枚举：

   ```sh
   system_profiler SPUSBDataType
   ```

6. 编译并运行 macOS UAC2 sender：

   ```sh
   ./r.sh build-uac2-macos
   UAC2_DEVICE_NAME="STM32H7B0 PCM5102A UAC2" ./r.sh run-uac2-macos
   ```

7. 串口应观察到：

   ```text
   UAC2 receiver start: STM32H7B0VBT6 FK7B0M1
   UAC2 stream ready
   UAC2 host audio
   UAC2 first packet
   UAC2 RX packets=... frames=... out_delivered=...
   ```

## 9. 判断问题所在

```text
macOS 看不到 USB 设备
    -> 先查 USB1/Type-C、PA11/PA12、VBUS、CC 下拉和 USB 线

USB 已枚举但没有 Audio MIDI 播放设备
    -> 查 UAC2 descriptors、device_next 配置和 alternate setting

terminal 已启用但 RX packets=0
    -> 查 USB OUT 协商、Type-C 物理链路和 UAC2 host 配置

RX packets 增长但 out_delivered=0
    -> 查 uac2_audio_stream 队列、sink 回调和 H7B0 lite 配置

out_delivered 增长但 PCM5102A 无声
    -> 查 PB13/PB12/PB15、PCM5102A DIN/BCK/LRCK、功放和模拟地

出现 I2S -EIO 或 TX slab allocation failed
    -> 查 I2S2 PLL1_Q、DMA/DMAMUX1、PCM5102A 时钟和输出队列
```

## 10. 当前状态

已完成：

- FK7B0M1 原理图 USB/USART/I2S 引脚确认
- H7B0 UAC2 overlay
- H7B0 WCH-DAPLink CMSIS-DAP 探测
- H7B0 固件烧录和校验
- H7B0 硬件复位下 SWD 调试识别
- H7B0 UAC2 固件编译验证

尚未完成：

- FK7B0M1 USB1 Type-C 实际 USB 枚举
- H7B0 macOS UAC2 音频播放
- H7B0 PCM5102A 声学输出
- H7B0 长时间时钟漂移和卡顿测试
