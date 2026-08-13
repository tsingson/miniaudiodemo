# STM32F401 Debug Serial and ST7735S Display

## 1. Debug serial

The default STM32F401 profile is the debug profile. It routes Zephyr console and log output to a USB CDC ACM device:

- USB D-: PA11
- USB D+: PA12
- macOS device: usually `/dev/cu.usbmodem*`
- baud rate used by the monitor: 115200

Connect the board USB data cable, flash the debug image, then check the device:

```sh
./r.sh build-demo
./r.sh flash-demo
ls /dev/cu.usbmodem* /dev/tty.usbmodem* 2>/dev/null
./r.sh monitor
```

The USB CDC device appears only after the firmware starts and the USB data connection is present. A charge-only cable will not enumerate. The ST-Link SWD connection is used for flashing and is not the USB CDC serial device.

The F401 overlay selects the internal HSI clock and configures the system PLL to 84 MHz. PLLQ provides the USB 48 MHz clock, so the running board does not depend on the ST-Link MCO clock after flashing.

The reusable debug settings are in `prj_debug.conf`. The default `prj.conf` also keeps debug logging enabled for direct west builds. `r.sh` explicitly adds `prj_debug.conf` to debug builds so the selected profile is visible in the build command.

For a hardware UART instead of USB CDC, use USART1:

- TX: PA9
- RX: PA10
- 115200 baud

Connect PA9 to the USB-UART adapter RX, PA10 to adapter TX, and share GND. This path does not create a macOS `/dev/cu.usbmodem*` device by itself; the adapter supplies its own `/dev/cu.usbserial*` device.

Production images use `prj_prod.conf`. They retain the display/audio drivers but disable application logs, console, serial, and USB CDC:

```sh
./r.sh build-demo-prod
./r.sh flash-demo-prod
```

## 2. ST7735S minimal English display

The display path is independent of the serial path. The board overlay provides:

- SPI1 SCK: PA5
- SPI1 MOSI/SDA: PA7
- SPI1 CS: PA3
- D/C: PA2
- RESET: PA6
- Backlight: PB1, active high

The display is configured as 128x160, write-only SPI, with a 4 MHz maximum SPI frequency. The demo entrypoint is `src/main_st7735s.c`.

The default lightweight renderer is `src/st7735s_log_display_stub.c`. It contains a small printable ASCII font. Each character occupies an 8x12 cell; the visible glyph is a compact 5x7 bitmap with padding. The renderer clears the screen to RGB565 white and draws glyphs in RGB565 black. Unsupported bytes are rendered as `?`.

Reuse the display module from another Zephyr entrypoint:

```c
#include "st7735s_log_display.h"

if (st7735s_log_display_init() == 0) {
    st7735s_log_display_line("Boot OK");
    st7735s_log_display_line("Audio ready");
}
```

The required device-tree contract is:

- `zephyr,display` points to the ST7735S node.
- `st7735s_ctrl.blk-gpios` describes the backlight.
- SPI1 and the ST7735S D/C, RESET, and CS pins match the board wiring.

Keep the lightweight renderer for STM32F401 builds to avoid pulling the larger ZPix font into the 256 KB image. The optional full font path is enabled with `-DMINIAUDIO_ST7735S_ZPIX_FONT=ON` on a target with sufficient flash.

## Regression check

Build the demo and inspect the generated device tree:

```sh
./r.sh build-demo
rg -n "spi1_sck_pa5|spi1_mosi_pa7|gpioa 0x3|st7735r@0" \
  build-zephyr-f401rct6-demo/zephyr/zephyr.dts
```

The build must generate `zephyr.elf`, `zephyr.bin`, and `zephyr.hex`. Hardware validation consists of seeing the USB CDC device on macOS and seeing a white screen with black ASCII text after flashing.
