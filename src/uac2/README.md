# src/uac2 — vendored/patched Zephyr STM32 UDC driver

This directory exists only because of one unmerged upstream fix. See
[../../zephyr_uac2_eval.md](../../zephyr_uac2_eval.md) for the full research
behind this decision.

## What's here

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
instrumented in [../usb_uac2_device.c](../usb_uac2_device.c).

## How it's wired in

- `CMakeLists.txt` (`MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN` branch) adds
  `src/uac2/udc_stm32.c` to `target_sources(app ...)` and adds
  `${ZEPHYR_BASE}/drivers/usb/udc` to the include path (needed for the
  private `"udc_common.h"`; `stm32_usb_common.h` is already globally
  available via Zephyr's own `zephyr_include_directories()`).
- `prj_uac2.conf` sets `CONFIG_UDC_STM32=n` so Zephyr's own in-tree
  `drivers/usb/udc/udc_stm32.c` is **not** also compiled (that would be a
  duplicate `DEVICE_DT_INST_DEFINE` for the same devicetree node), and
  manually re-selects everything `CONFIG_UDC_STM32` would otherwise have
  selected (`USE_STM32_LL_USB`, `USE_STM32_HAL_PCD`, `USE_STM32_HAL_PCD_EX`,
  `UDC_DRIVER_HAS_HIGH_SPEED_SUPPORT`, `STM32_USB_COMMON`, `PINCTRL`), since
  Kconfig `select` has no effect once the selecting symbol is off.
- Nothing else in the tree changes: `usbd_uac2.c` (the UAC2 class layer) and
  every other build variant (default `play_mcu.c`, `main_pcm5102_st7735s.c`,
  etc.) keep using Zephyr's stock, unmodified code. This only affects the
  `MINIAUDIO_PCM5102_ST7735S_UAC2_MAIN=ON` build (`./r.sh build-uac2-stm32`,
  `build-uac2-null`, `build-uac2-null-implicit`, `build-uac2-stm32-prod`).
- Applies equally to STM32F401 (`st,stm32-otgfs`) and STM32H7B0
  (`st,stm32-otghs` running Full-Speed) — both compatibles are handled by the
  same driver file.

## How to remove this vendoring later

Once `#113622` (or an equivalent fix) is merged into the Zephyr revision this
project builds against:

1. Delete `src/uac2/` entirely.
2. Remove the `src/uac2/udc_stm32.c` source entry and the
   `${ZEPHYR_BASE}/drivers/usb/udc` include directory from `CMakeLists.txt`.
3. Remove the `CONFIG_UDC_STM32=n` block (and its re-selected options) from
   `prj_uac2.conf`, restoring `CONFIG_UDC_STM32`'s own `default y`.
4. Rebuild `./r.sh build-uac2-stm32` and confirm it still links (Zephyr's own
   driver takes over again, unchanged from before this workaround existed).

## Known limitation

Only isochronous **OUT** (UAC2 playback/speaker) is fixed here, matching the
upstream PR. Isochronous **IN** (e.g. a UAC2 microphone) is left as an
upstream TODO because it needs a different fix shape (drop-and-rearm through
the normal data-in completion path, not a direct re-arm). This project only
uses UAC2 playback, so IN recovery is out of scope.
