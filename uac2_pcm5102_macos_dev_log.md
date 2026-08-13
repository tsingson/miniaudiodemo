# macOS UAC2 -> STM32F401 -> PCM5102A Development Log

## 1. Target architecture

The intended topology is:

```text
macOS WAV/DSP application
        |
        | CoreAudio/miniaudio selects a USB Audio playback device
        v
macOS USB host stack
        |
        | USB Audio Class 2 isochronous OUT
        v
STM32F401 UAC2 device
        |
        | PCM16 stereo frames
        v
STM32 I2S2 + PLLI2S + DMA
        |
        | DIN/BCK/LRCK
        v
PCM5102A -> LINE OUT -> amplifier
```

macOS is the USB host. STM32F401 is the USB Audio Class 2 device. The macOS application must not inject raw USB packets through libusb while the system audio driver owns the UAC2 interface. The standard approach is to select the STM32 USB Audio playback device through CoreAudio/miniaudio and let macOS schedule the isochronous packets.

## 2. Current implementation status

The first implementation slice is now present:

- `src/main_uac2_srv.c`: macOS sender entrypoint.
- `src/main_pcm5102_st7735s_uac2.c`: STM32 receiver entrypoint.
- `src/usb_uac2_device.h`: receiver interface.
- `src/usb_uac2_device.c`: explicit UAC2 protocol-layer placeholder.
- CMake targets and `r.sh` shortcuts for both sides.

The macOS sender is functional as a host-side audio-device selector. It enumerates playback devices, selects the device whose name contains `UAC2_DEVICE_NAME`, opens it at stereo float32/48 kHz, and loops the WAV file selected by `UAC2_INPUT_WAV`.

The STM32 receiver now uses Zephyr's experimental `device_next` UAC2 class. Its devicetree declares a playback-only Audio Function, programmable 48 kHz clock source, stereo USB input terminal, PCM5102A speaker output terminal, and an implicit-feedback AudioStreaming interface. The application registers `uac2_ops`, receives USB buffers, converts PCM16 frames to float, and hands them to the verified PCM5102A I2S writer.

At boot, `main_pcm5102_st7735s_uac2.c` first loops the embedded `3.wav` audio through PCM5102A. When the UAC2 terminal is enabled or valid host data arrives, the screen reports `UAC2 host audio` and the local source stops; subsequent PCM5102A blocks come from the macOS UAC2 stream. This makes the I2S and amplifier path testable before macOS is connected.

The remaining validation is hardware-facing: flash the receiver, confirm macOS enumerates the STM32 as a USB Audio playback device, run the sender, and verify acoustic output. The API is experimental and the current STM32F401 Full-Speed controller must be tested for endpoint scheduling and buffer timing.

## 3. New build shortcuts

macOS sender:

```sh
env -u ZEPHYR_BASE ./r.sh build-uac2-macos
UAC2_DEVICE_NAME="STM32" UAC2_INPUT_WAV=1.wav ./r.sh run-uac2-macos
```

        The sender also retains the existing DSP HTTP service. On successful startup it prints the selected UAC2 device and the control URL, for example `Connected to UAC2 playback device: ...` and `http://127.0.0.1:8080`.

The name is matched as a substring against the miniaudio playback-device list. The sender prints all playback devices when the requested name is not found.

STM32 receiver scaffold, debug:

```sh
./r.sh build-uac2-stm32
./r.sh flash-uac2-stm32
```

STM32 receiver scaffold, production:

```sh
./r.sh build-uac2-stm32-prod
./r.sh flash-uac2-stm32-prod
```

The existing direct WAV/PCM5102A path remains available through `build-pcm` and `flash-pcm`.

## 4. PCM5102A handoff contract

The existing verified PCM5102A writer remains the owner of the hardware output path:

- DIN: PB15 (`I2S2_SD`)
- BCK: PB13 (`I2S2_CK`)
- LRCK: PB12 (`I2S2_WS`)
- No MCLK/SCK
- No PCM5102A control GPIO handling

The UAC2 receiver must provide blocks of interleaved signed PCM16 stereo frames at 48 kHz to `play_mcu_write_frames()` after converting USB packet data into the application block size. The writer owns the I2S TX slab after `i2s_write()` succeeds and starts from a primed DMA queue to avoid underrun.

## 5. Required UAC2 protocol work

The native Zephyr UAC2 layer now provides the following; the project integration must validate each item on hardware:

1. Device/configuration descriptors with AudioControl and AudioStreaming interfaces.
2. Alternate setting 0 with no active stream and alternate setting 1 with the isochronous OUT endpoint.
3. UAC2 clock source descriptor and sample-rate control requests.
4. Feature/unit descriptors sufficient for a stereo PCM playback function.
5. Full-Speed isochronous OUT endpoint sized for 48 kHz stereo PCM16.
6. USB class request handling for interface alternate-setting changes and clock/sample-rate controls.
7. Endpoint callback that supplies aligned receive buffers and converts packet payloads into PCM blocks.
8. I2S handoff through the existing PCM5102A writer, with the writer retaining ownership of its own TX slab.
9. Hardware verification with macOS System Information and Audio MIDI Setup before acoustic testing.

The Full-Speed packet budget for stereo PCM16 at 48 kHz is approximately 192 bytes per millisecond. The implementation should account for USB frame packet scheduling and avoid assuming that every packet has exactly the same payload length.

## 6. Problems encountered before this UAC2 work

### USB disappeared when ST-Link was removed

The board initially used the Nucleo external HSE/MCO clock. The custom STM32F401 board depended on ST-Link while connected. The overlay was changed to use HSI and a system PLL at 84 MHz, with PLLQ providing the USB clock. The board then enumerated independently of ST-Link.

### ST7735S did not use the intended CS pin

The Nucleo base DTS supplied a default SPI1 CS. The actual screen wiring uses CS on PA3, so the overlay now explicitly sets SPI1 to SCK PA5, MOSI PA7, and CS PA3. The backlight is PB1, DC is PA2, and reset is PA6.

### PCM5102A produced no sound with a seemingly initialized I2S driver
        The USB Full-Speed packet size is about 48 stereo frames per millisecond (192 bytes). The PCM5102A writer uses 128-frame I2S blocks. The receiver therefore aggregates incoming USB packets in `usb_uac2_device.c` until 128 frames are available before calling `play_mcu_write_frames()`. Directly forwarding each 48-frame USB packet caused mismatched block timing and could leave the I2S path silent or underrun.

I2S2 initially lacked its independent PLLI2S audio clock. The overlay now enables PLLI2S and routes PLLI2S_R to I2S2. The PCM5102A path uses DIN/BCK/LRCK only.

### I2S TX queue errors

The first implementation exhausted TX slab blocks and later produced `I2S wr fail -5`. The cause was late DMA start combined with blocking ST7735S updates after starting the stream. The verified writer now primes four TX blocks before `I2S_TRIGGER_START` and does not synchronously update the display after audio starts.

## 7. Debugging checklist for the next slice

1. Build and flash the receiver scaffold.
2. Confirm the USB device enumerates as a USB device even before UAC2 audio streaming is implemented.
3. Add descriptors and verify that macOS lists an STM32 playback endpoint in Audio MIDI Setup.
4. Select the endpoint using `UAC2_DEVICE_NAME` in the macOS sender.
5. Confirm the receiver logs UAC2 alternate-setting and sample-rate requests.
6. Confirm ring-buffer packet counters increase while the sender runs.
7. Confirm the I2S writer receives PCM16 blocks without slab exhaustion or `-EIO`.
8. Connect PCM5102A LINE OUT to the amplifier and verify audio.

## 8. Current limitation

The native UAC2 class and project callbacks now compile into the STM32 image. Hardware enumeration and audio timing have not yet been verified in this session. If the STM32 does not enumerate, inspect the new USB device_next controller support and the generated UAC2 descriptors before changing the PCM5102A I2S path.
        ### UAC2 stream appeared ready but PCM5102A was silent

        The first native callback integration converted and forwarded each USB packet independently. At Full-Speed UAC2, a 48 kHz stereo PCM16 stream normally arrives as roughly 48 frames per 1 ms USB frame, while the verified PCM5102A writer consumes 128-frame blocks. The mismatch made the I2S producer timing fragile. The receiver now accumulates packets into exact 128-frame blocks before handing them to the I2S writer. Runtime checks should watch for the UAC2 terminal enabled event, continued `PCM5102A audio stream started`, and the absence of I2S `-EIO` or slab errors.
