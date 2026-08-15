# src/uac2 — UAC2 stream/device integration and patched STM32 UDC driver

This directory owns the reusable UAC2 stream bridge and protocol integration.
It also contains one vendored driver workaround; see
[../../zephyr_uac2_eval.md](../../zephyr_uac2_eval.md) for the research behind
that decision.

## What's here

- `uac2_audio_stream.c/.h`: public application-facing bridge for device
  startup, asynchronous pipeline handoff, and statistics snapshots.
- `usb_uac2_device.c/.h`: Zephyr UAC2 descriptors, endpoint callbacks, PCM16
  packet conversion, explicit feedback, and protocol counters.
- `udc_stm32.c`: a copy of `drivers/usb/udc/udc_stm32.c` **as pinned by this
  project's ZEPHYR_BASE (v4.4.2)**, with the fix from unmerged upstream PR
  [zephyrproject-rtos/zephyr#113622](https://github.com/zephyrproject-rtos/zephyr/pull/113622)
  applied on top (adapted from the PR's main-branch `stm32_pcd_handle_t`/
  `stm32_status_t` HAL2 abstraction back to v4.4.2's plain
  `PCD_HandleTypeDef`/`HAL_StatusTypeDef` types — the rest of the file is
  byte-for-byte the v4.4.2 driver).

## Why a full-file copy instead of a small add-on file

The fix (`HAL_PCD_ISOOUTIncompleteCallback` / `HAL_PCD_ISOINIncompleteCallback`)
needs `hpcd2data()` (a `#define` macro) and `struct udc_stm32_data`, both
private to `udc_stm32.c` (not declared in any header). There is no way to add
just the two new callbacks from a separate translation unit without
duplicating that private state, so the whole file has to be vendored.

## What the bug is

`udc_stm32.c` never implemented the HAL "isochronous incomplete transfer"
callbacks. When an isochronous OUT endpoint (UAC2 playback data path) misses a
single (micro)frame, the OTG core disables the endpoint and nothing ever
re-arms it — the receiver gets a few zero-length packets and then goes
permanently idle. This matches the `g_zero_packets` symptom already
instrumented in [usb_uac2_device.c](usb_uac2_device.c).

## How it's wired in

- `CMakeLists.txt` (`MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN` branch) adds
  `src/uac2/udc_stm32.c` to `target_sources(app ...)` and adds
  `${ZEPHYR_BASE}/drivers/usb/udc` to the include path (needed for the
  private `"udc_common.h"`; `stm32_usb_common.h` is already globally
  available via Zephyr's own `zephyr_include_directories()`).
- `prj_uac2.conf` sets `CONFIG_UDC_STM32=n` so Zephyr's own in-tree
  `drivers/usb/udc/udc_stm32.c` is **not** also compiled (that would be a
  duplicate `DEVICE_DT_INST_DEFINE` for the same devicetree node). The
  application `Kconfig` symbol `MINIAUDIO_UAC2_UDC_STM32_FIX` re-selects the
  dependencies and defaults normally supplied by `CONFIG_UDC_STM32`.
- Zephyr's `usbd_uac2.c` class layer remains stock. This only affects the
  `MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN=ON` build (`./r.sh build-uac2-stm32`,
  `build-uac2-implicit`, `build-uac2-null`, `build-uac2-null-implicit`,
  `build-uac2-stm32-prod`).
- Applies equally to STM32F401 (`st,stm32-otgfs`) and STM32H7B0
  (`st,stm32-otghs` running Full-Speed) — both compatibles are handled by the
  same driver file.

## How to remove this vendoring later

Once `#113622` (or an equivalent fix) is merged into the Zephyr revision this
project builds against:

1. Delete only `src/uac2/udc_stm32.c`; keep the stream/device integration
  files in this directory.
2. Remove the `src/uac2/udc_stm32.c` source entry and the
   `${ZEPHYR_BASE}/drivers/usb/udc` include directory from `CMakeLists.txt`.
3. Remove `CONFIG_UDC_STM32=n` from `prj_uac2.conf` and remove the now-unused
  `MINIAUDIO_UAC2_UDC_STM32_FIX` workaround from the application `Kconfig`,
  restoring `CONFIG_UDC_STM32`'s own defaults.
4. Rebuild `./r.sh build-uac2-stm32` and confirm it still links (Zephyr's own
   driver takes over again, unchanged from before this workaround existed).

## Known limitation

Only isochronous **OUT** (UAC2 playback/speaker) is fixed here, matching the
upstream PR. Isochronous **IN** (e.g. a UAC2 microphone) is left as an
upstream TODO because it needs a different fix shape (drop-and-rearm through
the normal data-in completion path, not a direct re-arm). This project only
uses UAC2 playback, so IN recovery is out of scope.
