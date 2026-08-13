# UAC2 Playback Implementation Plan

## Goal

Build and verify this path in three controlled phases:

```text
macOS/CoreAudio UAC2 host
    -> STM32F401 USB UAC2 device
    -> PCM16 stereo blocks
    -> STM32 I2S2/PLLI2S/DMA
    -> PCM5102A DIN/BCK/LRCK
```

The implementation must not claim success from USB enumeration alone. Audio success requires nonzero UAC2 payload, valid PCM frame accounting, I2S block delivery, and acoustic output.

## Phase 1: Independent PCM5102A playback

### Status

The reusable writer extraction is implemented:

- `src/pcm5102a_audio.c/.h` owns I2S2/PLLI2S/DMA, TX slab, priming, recovery, locking, and counters.
- `play_mcu_zephyr_pcm5102.c` now contains compatibility/runtime hooks instead of a second hardware writer.
- `src/main_pcm5102a_test.c` generates a deterministic stereo PCM16 440 Hz test signal.
- `./r.sh build-pcm5102a-test` builds the test image successfully.

The hardware acoustic gate is still pending; compilation alone does not prove the signal is audible.

On STM32F401, the hardware baseline test intentionally enables only the WAV source and PCM5102A output stage. The full DSP/plugin chain is available but must be enabled with a measured block-time budget; at 128 frames it can overrun the 2.67 ms real-time interval and cause I2S underruns.

The STM32 audio path now follows the same shape as the macOS path:

`source -> play_audio_pipeline -> DSP/plugin stages -> output stages`

UAC2 is currently a source callback, and the deterministic WAV/test generators use the same pipeline. The output fan-out accepts multiple stages, so Bluetooth, TF-card, and TCP readers can be added as source adapters without changing the PCM5102A node.

### Work

1. Extract the reusable PCM5102A writer from `src/play_mcu_zephyr_pcm5102.c` into:
   - `src/pcm5102a_audio.c`
   - `src/pcm5102a_audio.h`
2. Keep I2S2/PLLI2S/DMA configuration inside the module.
3. Expose a small API for:
   - initialization
   - interleaved float block write
   - optional PCM16 block write
   - status/error counters
4. Keep one serialized writer. Local playback and UAC2 callback must never write I2S concurrently.
5. Keep the existing `main_pcm5102_st7735s.c` behavior through the new API.
6. Add deterministic test sources:
   - PCM16 stereo, 48 kHz
   - PCM16 stereo, 96 kHz resampled to 48 kHz
   - mono duplicated to stereo
   - silence

### Data fixture rule

`3.wav` is only about 0.364 seconds at 96 kHz stereo PCM16 and is not a valid music-duration fixture. Do not add the large local `1.wav`/`3.wav` files to Git. Use a small generated tone or explicitly generated C fixture for firmware tests.

### Gate

Phase 1 passes only when:

- the standalone writer compiles and links;
- I2S write counters advance;
- no `-EIO`, underrun, or slab exhaustion occurs;
- the known test signal is distinguishable from repeated click/`ba` artifacts.

## Phase 2: Minimal UAC2 protocol and transport

### Device topology

Use the official Zephyr `device_next` UAC2 playback-only topology:

- one `zephyr,uac2` function;
- one fixed internal 48 kHz clock source;
- one USB streaming input terminal;
- one output terminal;
- one AudioStreaming interface;
- Full-Speed isochronous OUT;
- explicit feedback for the initial Apple compatibility test.

UAC2 supports bidirectional audio, but a PCM5102A playback-only device does not need a fake microphone/IN endpoint. Add an IN endpoint only in a separate bidirectional test target.

### Work

1. Use `CONFIG_USB_DEVICE_STACK_NEXT=y` and `CONFIG_USBD_AUDIO2_CLASS=y`.
2. Keep CDC disabled in the pure UAC2 test image; route diagnostics to USART1 or ST7735S.
3. Register all mandatory `uac2_ops` callbacks:
   - SOF
   - terminal update
   - receive buffer allocation
   - data receive
   - buffer release if required by the class
   - explicit feedback
4. Verify the control sequence:
   - USB enumeration
   - AudioControl descriptor read
   - Clock Source `GET_CUR`
   - optional `SET_CUR`
   - `SET_INTERFACE` alternate setting 1
   - terminal enabled
   - SOF and feedback
   - isochronous OUT payload
5. Add a macOS C protocol test sender using CoreAudio/miniaudio and a deterministic PCM sequence/checksum.
6. Add bounded STM32 counters for control, SOF, buffer requests/rejections, packet bytes, PCM frames, checksum, and callback errors.

### Gate

Do not enter Phase 3 until STM32 proves:

```text
SET_INTERFACE alt=1
terminal enabled
SOF active
RX packets > 0
PCM checksum matches expected test sequence
```

## Phase 3: Audio playback

1. Aggregate Full-Speed PCM16 packets into exact 128-frame I2S blocks.
2. Keep PCM5102A output fixed at 48 kHz until non-48 kHz resampling is separately verified.
3. Switch from local fallback to UAC2 only after the first valid nonzero payload.
4. Use one writer queue/mutex so local and UAC2 sources cannot interleave.
5. Verify `LINE OUT -> amplifier` acoustically.
6. Repeat with production configuration: no serial/log output, key milestones on ST7735S.

## Shortcuts

```sh
# macOS host
./r.sh build-uac2-macos
UAC2_DEVICE_NAME="STM32F401 PCM5102A UAC2" ./r.sh run-uac2-macos

# STM32 pure UAC2 debug
./r.sh build-uac2-stm32
./r.sh flash-uac2-stm32

# STM32 production
./r.sh build-uac2-stm32-prod
./r.sh flash-uac2-stm32-prod
```

## Stop conditions

Stop and diagnose at the first failed layer:

- `RX packets=0`: do not modify PCM/I2S; inspect UAC2 control/alternate-setting.
- `RX packets>0`, `I2S blocks=0`: inspect packet aggregation and ownership.
- `I2S blocks>0`, no sound: inspect I2S/PLLI2S/PCM5102A hardware path.
- repeated `I2S wr fail -5`: inspect producer concurrency and underrun timing.

## Final review checklist

- `git diff --check`
- `zsh -n r.sh`
- host sender build
- debug receiver build
- production receiver build
- generated descriptor/config inspection
- hardware enumeration
- UAC2 packet/checksum test
- PCM5102A acoustic test
- no accidental commit of local WAV files
