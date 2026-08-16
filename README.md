
#  dsp

## mcu
stm32f401RCT6
arm cortex-m4 84mhz 有 fpu单精度, mpu , 256kb闪存, 64kb sram, 一个12位 adc, 两个32位定时器 

上电中, 按 boot0 / 复位锓, 松开复位键, 0.5秒松开 boot0 
断电后, 按 boot0 上电 后 0.5秒松开 boot0

默认构建为调试模式，使用 USB CDC ACM；生产模式关闭日志、控制台和串口，但保留显示/音频驱动。UAC2 构建例外：`prj_uac2.conf` 关闭 USB CDC，并由 overlay 把日志路由到 USART1（PA9/PA10，115200）。
PCM5102A 调试过程、故障现象与最终三线 I2S 配置见：[pcm5102a_dev_log.md](pcm5102a_dev_log.md)。
macOS UAC2 到 STM32F401/PCM5102A 的架构、当前实现状态和后续协议工作见：[uac2_pcm5102_macos_dev_log.md](uac2_pcm5102_macos_dev_log.md)。

UAC2 快捷命令：`./r.sh build-uac2-macos` 构建 macOS sender，`UAC2_DEVICE_NAME="STM32" ./r.sh run-uac2-macos` 运行 sender；STM32 可用 `build/flash-uac2-stm32`（显式反馈）或 `build/flash-uac2-implicit`（隐式反馈）构建和烧录。

FK7B0M1 STM32H7B0 核心板使用独立入口：`./r.sh build-uac2-h7b0` / `./r.sh flash-uac2-h7b0`。该目标共享 UAC2、异步队列和 PCM5102A 输出代码，但由于 H7B0VBT6 的 128KB Flash，不编入本地 WAV fallback 和完整 DSP 插件链；USB UAC2 PCM16/48kHz 到 PCM5102A 播放功能保持一致。

USART1 (PA9/PA10, 115200) 可作为硬件后备串口。生产模式使用 `prj_prod.conf`，关闭日志、控制台和 USB CDC。



## 音频 codec

zephyr 4.4.2 控制 PCM5102A, 引脚是 sck / bck / din / lrck / vin / gnd, 及 FLT / demp / xsmt / FMT / a3v3 / agnd / rout / agnd / lrout 引脚, 另有 line out 接口。

PCM5102A + ST7735S 当前入口为 `src/main_pcm5102_st7735s.c`，音频发送由 `src/pcm5102/play_mcu_zephyr_pcm5102.c` 提供 Zephyr I2S 适配。

### Zephyr 编译（已验证可通过）

```sh
/Users/qinshen/go/zephyrproject/.venv/bin/west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine
```

若仅运行 ST7735S 白底黑字 `hello copilot` 演示入口（`src/main_st7735s.c`）：

```sh
/Users/qinshen/go/zephyrproject/.venv/bin/west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine -- -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
```

若运行 PCM5102 + ST7735S 联合入口（`src/main_pcm5102_st7735s.c`，循环播放内嵌 WAV 并经过 DSP pipeline）：

```sh
/Users/qinshen/go/zephyrproject/.venv/bin/west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6-pcm5102-st7735s --pristine -- -DMINIAUDIO_PCM5102_ST7735S_MAIN=ON
```

当前该入口已改为内嵌并循环播放 `./3.wav` 到 PCM5102A（WAV PCM16，支持 mono/stereo；若源采样率不是 48kHz，会在固件内按最近邻方式重采样到 48kHz 输出）。I2S 仅使用 DIN/BCK/LRCK 三根线，不配置 MCLK 或 PCM5102A 控制 GPIO。

若做 STM32H7B0VBT6 串口对比测试入口（`src/main_stm32h7b0vbt6.c`，打印 `hello copilot`）：

```sh
/Users/qinshen/go/zephyrproject/.venv/bin/west build -b mini_stm32h7b0 . --build-dir build-zephyr-h7b0vbt6 --pristine -- -DMINIAUDIO_STM32H7B0VBT6_DEMO_MAIN=ON -DCONF_FILE=prj_h7b0_demo.conf
```

说明：
- 默认使用轻量显示日志实现（不编入 `zpix12_font_data.c`），用于适配 STM32F401RCT6 的 256KB Flash。
- 轻量模式下，ST7735S 使用 8x12 字符单元显示英文/ASCII；中文会退化为 `?`（避免大字库占用）。
- 若你换到更大 Flash 容量芯片并需要中文字库，可开启：

```sh
/Users/qinshen/go/zephyrproject/.venv/bin/west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine -- -DMINIAUDIO_ST7735S_ZPIX_FONT=ON
```

overlay 文件：`boards/nucleo_f401re.overlay`（内部 include `boards/stm32f401rct6.overlay`）

### ST7735S 英文显示复用

轻量显示模块 `src/lcd/st7735s_log_display_stub.c` 使用 8x12 像素字符单元，内含完整可打印 ASCII 字符表；实际笔画为紧凑的 5x7 位图并在单元内留白，默认白底黑字。未知或非 ASCII 字节显示为 `?`。

复用方式：

```c
if (st7735s_log_display_init() == 0) {
	st7735s_log_display_line("Hello Copilot");
}
```

该显示模块独立于 USB 串口，生产模式仍可保留屏幕显示；设备树需要保留 `zephyr,display` 和 `st7735s_ctrl.blk-gpios`。

### 一键脚本

```sh
./r.sh build      # 编译
./r.sh flash      # 烧录
./r.sh monitor    # 串口监测（115200）
./r.sh all        # 编译 + 烧录 + 串口监测
./r.sh build-demo # 编译 ST7735S demo 入口（src/main_st7735s.c）
./r.sh flash-demo # 烧录 ST7735S demo 固件
./r.sh all-demo   # 编译 + 烧录 ST7735S demo + 串口监测
./r.sh build-prod # 编译生产模式，关闭 USB 调试串口
./r.sh flash-prod # 烧录生产模式固件
./r.sh build-demo-prod # 编译 ST7735S demo 生产固件，关闭调试串口
./r.sh flash-demo-prod # 烧录 ST7735S demo 生产固件
./r.sh build-pcm  # 编译 PCM5102+ST7735S 内嵌 WAV/DSP 入口（src/main_pcm5102_st7735s.c）
./r.sh flash-pcm  # 烧录 PCM5102+ST7735S 音频固件
./r.sh all-pcm    # 编译 + 烧录 PCM5102+ST7735S + 串口监测
./r.sh build-pcm-prod # 编译 PCM5102A+ST7735S 生产固件
./r.sh flash-pcm-prod # 烧录 PCM5102A+ST7735S 生产固件
./r.sh build-pcm5102a-test # 编译确定性 PCM5102A 写入测试固件（src/main_pcm5102a_test.c）
./r.sh flash-pcm5102a-test # 烧录确定性 PCM5102A 写入测试固件
./r.sh build-uac2-macos # 编译 macOS UAC2 sender（src/main_uac2_srv.c）
./r.sh run-uac2-macos   # 运行 macOS UAC2 sender（设置 UAC2_DEVICE_NAME）
./r.sh build-uac2-stm32 # 编译 STM32 原生 Zephyr UAC2 receiver + PCM5102A（USART1 日志，无 LCD）
./r.sh flash-uac2-stm32 # 烧录 STM32 UAC2 receiver 固件
./r.sh build-uac2-implicit # 编译隐式反馈 UAC2 receiver + PCM5102A
./r.sh flash-uac2-implicit # 烧录隐式反馈 UAC2 receiver
./r.sh build-uac2-null  # 编译 UAC2 协议测试固件（不带 PCM5102A 输出）
./r.sh flash-uac2-null  # 烧录 UAC2 协议测试固件
./r.sh build-uac2-null-implicit # 编译 UAC2 隐式反馈 null-sink 固件
./r.sh flash-uac2-null-implicit # 烧录 UAC2 隐式反馈 null-sink 固件
./r.sh build-uac2-stm32-prod # 编译 STM32 UAC2 生产固件
./r.sh flash-uac2-stm32-prod # 烧录 STM32 UAC2 生产固件
./r.sh clean      # 清理 build/demo/demo-prod/pcm/pcm-prod 临时构建目录
```

避免烧错固件：
- `build/flash/all` 使用默认入口（`src/play_mcu.c`）。
- `build-demo/flash-demo/all-demo` 使用 ST7735S 演示入口（`src/main_st7735s.c`）。
- 默认 `prj.conf` 是调试模式；生产模式使用 `prj_prod.conf`。
- `build-pcm/flash-pcm/all-pcm` 使用 PCM5102+ST7735S 内嵌 WAV/DSP 入口（`src/main_pcm5102_st7735s.c`）。
- `build-pcm5102a-test/flash-pcm5102a-test` 使用确定性 PCM16/48kHz 写入测试入口，不带 DSP 管线。
- `build-uac2-stm32(-prod)`、`build-uac2-implicit`、`build-uac2-null` 和 `build-uac2-null-implicit` 均使用原生 Zephyr `zephyr,uac2` 接收入口；`src/uac2/uac2_audio_stream.c` 统一封装异步队列、设备初始化和统计，`src/uac2/usb_uac2_device.c` 只负责协议与端点。项目已不再包含任何 TinyUSB 代码路径。
- `build-uac2-h7b0` 使用 `mini_stm32h7b0` board 和 `boards/stm32h7b0vbt6_uac2.overlay`；FK7B0M1 的 PCM5102A 三线 I2S 使用 PB13/BCK、PB12/LRCK、PB15/DIN，USB 使用 PA11/PA12，日志使用 USART1 PA9/PA10。
- `clean` 只清理 `build/demo/demo-prod/pcm/pcm-prod` 五个目录，不清理 UAC2/PCM5102A 测试/macOS 相关构建目录，需要时手动 `rm -rf build-zephyr-f401rct6-uac2*  build-zephyr-f401rct6-pcm5102a-test build-macos-uac2`。

### 当前 I2S/控制脚映射

- I2S2_CK(PCM5102A BCK): PB13
- I2S2_SD(PCM5102A DIN): PB15
- I2S2_WS(PCM5102A LRCK): PB12
说明：
- 你的板子未给出 PC6，可不接 PCM5102A 的 SCK(MCLK)，当前按 3 线 I2S 运行（BCK/LRCK/DIN）。
- VIN/GND/A3V3/AGND/ROUT/LROUT/Line Out 为模拟/电源连线，不在 DTS 中以数字外设配置。
- 当前固件不配置 FLT/DEMP/XSMT/FMT GPIO；这些模块脚应按 PCM5102A 模块硬件要求固定到所需电平。

## ST7735S 显示移植

已从 `lcddemo` 迁移以下内容到当前工程（未引入任何图片 C 数组）：

- 显示驱动接入：使用 Zephyr 原生 `sitronix,st7735r`(兼容 ST7735S) + `zephyr,mipi-dbi-spi`
- overlay 参考：`boards/stm32f401rct6.overlay`
- 中文字库：`src/lcd/zpix12_font_data.c` + `src/lcd/zpix12_font_data.h`
- 运行日志显示模块：`src/lcd/st7735s_log_display.c` + `src/lcd/st7735s_log_display.h`
- 运行日志默认样式：白底黑字（便于在常见 ST7735S 模组上确认点亮与文本可见性）

### STM32F401 上的 ST7735S 引脚配置（当前）

- SCL (SPI1_SCK): PA5
- SDA (SPI1_MOSI): PA7
- RES (ST7735S_RST): PA6
- DC (ST7735S_DC): PA2
- CS (SPI1_CS): PA3
- BLK (背光): PB1

说明：
- MISO 未使用（write-only），显示输出通过 `zephyr,display` 设备在运行时初始化。
- 当前配置分辨率为 128x160（x-offset=2, y-offset=1）。
- 为提高连线稳定性，`mipi-max-frequency` 当前设置为 4MHz（若后续稳定可再升回 8MHz）。

### 板载硬件澄清

- 按键：BOOT0 / NRST / KEY
- 指示灯：PWR 灯 + 1 个 LED 灯
- 调试下载接口（ST-Link v2）：SW / 3V3 / DIO / SCK / GND
- 说明：此前“C13 接 LED”信息已作废，C13 更可能是板上电容位号，不作为 GPIO 引脚使用。

## STM32F401RCT6 引脚配置总表

### 控制台日志（USB CDC，默认）

- PA11: USB_DM (D-)
- PA12: USB_DP (D+)
- macOS 设备：`/dev/cu.usbmodem*`

### 后备串口（USART1）

- PA9: USART1_TX（接 USB-UART RX）
- PA10: USART1_RX（接 USB-UART TX）
- 波特率：115200

### 音频输出（PCM5102A, I2S2 3线）

- PB13: I2S2_CK -> PCM5102A BCK
- PB12: I2S2_WS -> PCM5102A LRCK
- PB15: I2S2_SD -> PCM5102A DIN

### LCD（ST7735S, SPI1）

- PA7: SDA (SPI1_MOSI)
- PA6: RES (ST7735S_RST)
- PA2: DC (ST7735S_DC)
- PA3: CS (SPI1_CS)
- PB1: BLK (背光控制，高电平点亮)

### USB 设备（用于 macOS 识别）

- PA11: USB_DM (D-)
- PA12: USB_DP (D+)
- 默认调试固件启用 USB CDC ACM，连接后 macOS 会出现 `/dev/cu.usbmodem*`；UAC2 固件关闭 CDC，将同一组 USB 引脚用于 UAC2，并通过 USART1 输出日志。
- 注意：PA11/PA12 不能再用于 ST7735S 的 RES/BLK。

### 下载调试（ST-Link v2, SWD）

- SWDIO: DIO
- SWCLK: SCK
- GND: GND
- VTref: 3V3

### 板载按键/电源

- 按键：BOOT0 / NRST / KEY
- 指示：PWR 灯 + 1 个用户 LED


