# PCM5102A Development Log

## Scope

Target: STM32F401RCT6 + PCM5102A + ST7735S under Zephyr 4.4.1.

The verified application entrypoint is `src/main_pcm5102_st7735s.c`.

## Final wiring

PCM5102A digital audio uses only the three I2S signals:

- DIN: STM32 PB15 (`I2S2_SD`)
- BCK: STM32 PB13 (`I2S2_CK`)
- LRCK: STM32 PB12 (`I2S2_WS`)

No MCLK/SCK is connected. No PCM5102A control GPIO is configured by firmware. `LINE OUT` is connected to the power amplifier input, with a shared analog ground.

The PCM5102A does not need an I2C/SPI register initialization sequence. The STM32 I2S peripheral does need runtime configuration and DMA startup. The firmware configures stereo 16-bit I2S at 48 kHz and makes STM32 the BCK/LRCK controller.

## Debug and production profiles

Debug mode is the default development path:

```sh
./r.sh build-pcm
./r.sh flash-pcm
./r.sh monitor
```

Debug mode enables USB CDC ACM, Zephyr logs, and the console. macOS normally exposes the device as `/dev/cu.usbmodem*`.

Production mode keeps the display and audio drivers but disables serial, console, USB CDC, and application logs:

```sh
./r.sh build-pcm-prod
./r.sh flash-pcm-prod
```

No macOS serial device in production mode is expected.

## ST7735S behavior

The display uses SPI1:

- SCK: PA5
- MOSI/SDA: PA7
- CS: PA3
- DC: PA2
- RESET: PA6
- BLK: PB1, active high

The lightweight renderer uses an 8x12 character cell with a compact 5x7 ASCII bitmap. It clears the screen to white and draws black text. In the PCM entrypoint, the display is reserved for key milestones and errors, not the high-rate audio loop.

## Investigation history

### 1. Debug serial did not enumerate after ST-Link was removed

Symptom: the firmware ran while ST-Link was connected, but after ST-Link was disconnected macOS showed no USB device and no `/dev/cu.usbmodem*` port. The ST7735S also stopped at a blank screen.

Cause: the board was using the Nucleo default external HSE/MCO clock. The custom STM32F401 board did not have a reliable external clock after ST-Link was removed. USB requires a stable 48 MHz clock, and the display SPI timing also depends on the system clock.

Fix: `boards/stm32f401rct6.overlay` now disables HSE, enables HSI, configures the system PLL for 84 MHz, and provides the USB clock through PLLQ. The board can run independently of ST-Link.

### 2. ST7735S was white or showed invalid output

The final device-tree wiring was made explicit instead of relying on the Nucleo defaults. In particular, CS is PA3, not the Nucleo default PB6, and SPI1 uses only SCK and MOSI. The backlight is PB1.

The old demo entrypoint also contained a duplicate stale drawing block. It was removed so `main_st7735s.c` uses the reusable `st7735s_log_display` module as its single rendering path.

### 3. `No TX slab block: -11`

Symptom: I2S initialization succeeded, then the log repeatedly reported `No TX slab block: -11`.

Cause: the application producer filled all TX slab blocks before the DMA stream had a chance to consume them. The TX path also started too late.

Fix: the application uses the Zephyr STM32 I2S ownership model: successful `i2s_write()` transfers ownership of the slab block to the I2S driver, which frees it after DMA completion. The application waits for a slab block instead of timing out after 100 ms.

### 4. `audio wr fail -5`

Symptom: the display showed `I2S wr fail -5`, where `-5` is `-EIO`.

Cause: the first block was started immediately, then the application performed a relatively slow series of ST7735S SPI writes for the `Audio stream started` message. The audio DMA underrun put the STM32 I2S stream into its error state.

Fix: four audio blocks are queued before `I2S_TRIGGER_START`. The pre-start display message is written before the clock starts. After the stream starts, the audio loop does not synchronously update the display.

### 5. No sound despite successful I2S initialization

Symptom: the driver logged that I2S initialized, but the PCM5102A produced no audio.

Cause: I2S2 had only the APB1 peripheral clock. STM32F401 I2S audio timing requires the independent PLLI2S clock path.

Fix: the overlay enables PLLI2S from HSI and routes PLLI2S_R to I2S2. The generated device tree must show two I2S2 clocks: the APB1 peripheral clock and the PLLI2S audio clock.

## Useful validation commands

```sh
# Debug image
./r.sh build-pcm
./r.sh flash-pcm
./r.sh monitor

# Production image
./r.sh build-pcm-prod
./r.sh flash-pcm-prod

# Confirm the I2S clock sources
rg -n "i2s2:|plli2s:|PLLI2S|I2S_SEL" \
  build-zephyr-f401rct6-pcm5102-st7735s/zephyr/zephyr.dts
```

Expected runtime milestones include:

```text
PCM5102A I2S TX initialized @ 48000 Hz (DIN/BCK/LRCK)
PCM5102A audio stream started
```

The absence of `No TX slab block` and `I2S wr fail -5` indicates that the TX queue is continuing to recycle blocks. Actual acoustic output still depends on PCM5102A power, analog ground, line-out wiring, and the amplifier input level.
