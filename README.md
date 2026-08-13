
#  dsp

## mcu
stm32f401RCT6
arm cortex-m4 84mhz 有 fpu单精度, mpu , 256kb闪存, 64kb sram, 一个12位 adc, 两个32位定时器 

上电中, 按 boot0 / 复位锓, 松开复位键, 0.5秒松开 boot0 
断电后, 按 boot0 上电 后 0.5秒松开 boot0

STM32F401 调试串口与 ST7735S 英文显示的独立说明见：[docs/stm32f401_debug_display.md](docs/stm32f401_debug_display.md)。默认构建为调试模式，使用 USB CDC ACM；生产模式关闭日志、控制台和串口，但保留显示/音频驱动。

USART1 (PA9/PA10, 115200) 可作为硬件后备串口。生产模式使用 `prj_prod.conf`，关闭日志、控制台和 USB CDC。



## 音频 codec

zephyr 4.4.2 控制 PCM5102A, 引脚是 sck / bck / din / lck / vin / gnd, 及 FLT / demp / xsmt / FMT / a3v3 / agnd / rout / agnd / lrout 引脚, 另有 line out 接口。

当前工程已配置为使用 `src/play_mcu.c` 作为入口，并通过 `src/play_mcu_zephyr_pcm5102.c` 提供 Zephyr I2S/GPIO 适配。

### Zephyr 编译（已验证可通过）

```sh
west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine
```

若仅运行 ST7735S 白底黑字 `hello copilot` 演示入口（`src/main_st7735s.c`）：

```sh
west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine -- -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
```

若运行 PCM5102 + ST7735S 联合入口（`src/main_pcm5102_st7735s.c`，输出规则噪音并双通道日志）：

```sh
west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6-pcm5102-st7735s --pristine -- -DMINIAUDIO_PCM5102_ST7735S_MAIN=ON
```

当前该入口已改为内嵌并循环播放 `./3.wav`（WAV PCM16，支持 mono/stereo；若源采样率不是 48kHz，会在固件内按最近邻方式重采样到 48kHz 输出）。

若做 STM32H7B0VBT6 串口对比测试入口（`src/main_stm32h7b0vbt6.c`，打印 `hello copilot`）：

```sh
west build -b mini_stm32h7b0 . --build-dir build-zephyr-h7b0vbt6 --pristine -- -DMINIAUDIO_STM32H7B0VBT6_DEMO_MAIN=ON -DCONF_FILE=prj_h7b0_demo.conf
```

说明：
- 默认使用轻量显示日志实现（不编入 `zpix12_font_data.c`），用于适配 STM32F401RCT6 的 256KB Flash。
- 轻量模式下，ST7735S 使用 8x12 字符单元显示英文/ASCII；中文会退化为 `?`（避免大字库占用）。
- 若你换到更大 Flash 容量芯片并需要中文字库，可开启：

```sh
west build -b nucleo_f401re . --build-dir build-zephyr-f401rct6 --pristine -- -DMINIAUDIO_ST7735S_ZPIX_FONT=ON
```

overlay 文件：`boards/nucleo_f401re.overlay`（内部 include `boards/stm32f401rct6.overlay`）

### ST7735S 英文显示复用

轻量显示模块 `src/st7735s_log_display_stub.c` 使用 8x12 像素字符单元，内含完整可打印 ASCII 字符表；实际笔画为紧凑的 5x7 位图并在单元内留白，默认白底黑字。未知或非 ASCII 字节显示为 `?`。

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
./r.sh build-pcm  # 编译 PCM5102+ST7735S 规则噪音入口（src/main_pcm5102_st7735s.c）
./r.sh flash-pcm  # 烧录 PCM5102+ST7735S 规则噪音固件
./r.sh all-pcm    # 编译 + 烧录 PCM5102+ST7735S + 串口监测
./r.sh clean      # 清理 ./build-zephyr-f401rct6 临时目录
```

避免烧错固件：
- `build/flash/all` 使用默认入口（`src/play_mcu.c`）。
- `build-demo/flash-demo/all-demo` 使用 ST7735S 演示入口（`src/main_st7735s.c`）。
- 默认 `prj.conf` 是调试模式；生产模式使用 `prj_prod.conf`。
- `build-pcm/flash-pcm/all-pcm` 使用 PCM5102+ST7735S 规则噪音入口（`src/main_pcm5102_st7735s.c`）。

### 当前 I2S/控制脚映射

- I2S2_CK(PCM5102A BCK): PB13
- I2S2_SD(PCM5102A DIN): PB15
- I2S2_WS(PCM5102A LCK): PB12
- FLT: PA0
- DEMP: PA1
- XSMT: PA4
- FMT: PA8

说明：
- 你的板子未给出 PC6，可不接 PCM5102A 的 SCK(MCLK)，当前按 3 线 I2S 运行（BCK/LCK/DIN）。
- VIN/GND/A3V3/AGND/ROUT/LROUT/Line Out 为模拟/电源连线，不在 DTS 中以数字外设配置。
- PCM5102 控制脚默认电平：`FLT=1(低延迟)`、`DEMP=0(关闭去加重)`、`FMT=0(I2S)`、`XSMT=1(取消静音)`。

## ST7735S 显示移植

已从 `lcddemo` 迁移以下内容到当前工程（未引入任何图片 C 数组）：

- 显示驱动接入：使用 Zephyr 原生 `sitronix,st7735r`(兼容 ST7735S) + `zephyr,mipi-dbi-spi`
- overlay 参考：`boards/stm32f401rct6.overlay`
- 中文字库：`src/zpix12_font_data.c` + `src/zpix12_font_data.h`
- 运行日志显示模块：`src/st7735s_log_display.c` + `src/st7735s_log_display.h`
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
- PB12: I2S2_WS -> PCM5102A LCK
- PB15: I2S2_SD -> PCM5102A DIN
- PA0: FLT 控制
- PA1: DEMP 控制
- PA4: XSMT 控制
- PA8: FMT 控制

### 显示屏（ST7735S, SPI1）

- PA5: SCL (SPI1_SCK)
- PA7: SDA (SPI1_MOSI)
- PA6: RES (ST7735S_RST)
- PA2: DC (ST7735S_DC)
- PA3: CS (SPI1_CS)
- PB1: BLK (背光控制，高电平点亮)

### USB 设备（用于 macOS 识别）

- PA11: USB_DM (D-)
- PA12: USB_DP (D+)
- 已启用 Zephyr USB CDC ACM，连接后 macOS 会出现 `/dev/cu.usbmodem*` 设备。
- 注意：PA11/PA12 不能再用于 ST7735S 的 RES/BLK。

### 下载调试（ST-Link v2, SWD）

- SWDIO: DIO
- SWCLK: SCK
- GND: GND
- VTref: 3V3

### 板载按键/电源

- 按键：BOOT0 / NRST / KEY
- 指示：PWR 灯 + 1 个用户 LED


