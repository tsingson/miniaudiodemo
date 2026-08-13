#!/usr/bin/env zsh
set -euo pipefail

BOARD="nucleo_f401re"
BUILD_DIR="build-zephyr-f401rct6"
BUILD_DIR_DEMO="build-zephyr-f401rct6-demo"
BUILD_DIR_DEMO_PROD="build-zephyr-f401rct6-demo-prod"
BUILD_DIR_PCM="build-zephyr-f401rct6-pcm5102-st7735s"
BUILD_DIR_PCM_PROD="build-zephyr-f401rct6-pcm5102-st7735s-prod"
BAUD="115200"

WEST="/Users/qinshen/go/zephyrproject/.venv/bin/west"
usage() {
  cat <<'EOF'
Usage: ./r.sh <build|flash|monitor|all|build-demo|flash-demo|all-demo|build-demo-prod|flash-demo-prod|build-prod|flash-prod|build-pcm|flash-pcm|all-pcm|build-pcm-prod|flash-pcm-prod|build-pcm5102a-test|flash-pcm5102a-test|build-uac2-macos|run-uac2-macos|build-uac2-stm32|flash-uac2-stm32|build-uac2-stm32-prod|flash-uac2-stm32-prod|clean>

  build    Configure and build Zephyr app
  flash    Flash firmware to board via west
  monitor  Open serial monitor at 115200 baud
  all      build + flash + monitor
  build-demo  Build ST7735S demo main (src/main_st7735s.c)
  flash-demo  Flash ST7735S demo build
  all-demo    build-demo + flash-demo + monitor
  build-demo-prod  Build ST7735S demo production image without debug serial
  flash-demo-prod  Flash ST7735S demo production image
  build-prod  Build production profile with debug serial disabled
  flash-prod  Flash production profile
  build-pcm   Build PCM5102A + ST7735S audio demo (src/main_pcm5102_st7735s.c)
  flash-pcm   Flash PCM5102A + ST7735S audio demo build
  all-pcm     build-pcm + flash-pcm + monitor
  build-pcm-prod  Build PCM5102A + ST7735S production image without debug serial
  flash-pcm-prod  Flash PCM5102A + ST7735S production image
  build-pcm5102a-test  Build deterministic PCM5102A writer test
  flash-pcm5102a-test  Flash deterministic PCM5102A writer test
  build-uac2-macos  Build the macOS UAC2 sender
  run-uac2-macos    Run the macOS UAC2 sender (set UAC2_DEVICE_NAME)
  build-uac2-stm32  Build the STM32 UAC2 receiver scaffold
  flash-uac2-stm32  Flash the STM32 UAC2 receiver scaffold
  build-uac2-stm32-prod  Build the STM32 UAC2 production scaffold
  flash-uac2-stm32-prod  Flash the STM32 UAC2 production scaffold
  clean    Remove temporary Zephyr build directory
EOF
}

build_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf
}

build_demo_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR_DEMO" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
}

build_demo_prod_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR_DEMO_PROD" --pristine -- -DCONF_FILE=prj_prod.conf -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
}

build_prod_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR" --pristine -- -DCONF_FILE=prj_prod.conf
}

build_pcm_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR_PCM" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf -DMINIAUDIO_PCM5102_ST7735S_MAIN=ON
}

build_pcm_prod_app() {
  "$WEST" build -b "$BOARD" . --build-dir "$BUILD_DIR_PCM_PROD" --pristine -- -DCONF_FILE=prj_prod.conf -DMINIAUDIO_PCM5102_ST7735S_MAIN=ON
}

build_pcm5102a_test_app() {
  "$WEST" build -b "$BOARD" . --build-dir build-zephyr-f401rct6-pcm5102a-test --pristine -- -DEXTRA_CONF_FILE=prj_pcm5102a_test.conf -DMINIAUDIO_PCM5102A_TEST_MAIN=ON
}

build_uac2_macos_app() {
  env -u ZEPHYR_BASE -u ZEPHYR_TOOLCHAIN_VARIANT cmake -S . -B build-macos-uac2 -DPLAY_MACOS_USE_CMSIS_DSP=OFF
  env -u ZEPHYR_BASE -u ZEPHYR_TOOLCHAIN_VARIANT cmake --build build-macos-uac2 --target main_uac2_srv
}

run_uac2_macos_app() {
  ./build-macos-uac2/main_uac2_srv
}

build_uac2_stm32_app() {
  "$WEST" build -b "$BOARD" . --build-dir build-zephyr-f401rct6-uac2 --pristine -- -DEXTRA_CONF_FILE=prj_uac2.conf -DDTC_OVERLAY_FILE=boards/stm32f401rct6_uac2.overlay -DMINIAUDIO_PCM5102_ST7735S_UAC2_MAIN=ON
}

build_uac2_stm32_prod_app() {
  "$WEST" build -b "$BOARD" . --build-dir build-zephyr-f401rct6-uac2-prod --pristine -- -DCONF_FILE=prj_prod.conf -DEXTRA_CONF_FILE=prj_uac2.conf -DMINIAUDIO_PCM5102_ST7735S_UAC2_MAIN=ON
}

flash_app() {
  local build_dir="${1:-$BUILD_DIR}"
  local cube_cli_macos="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_Programmer_CLI"
  local cube_cli_macos_alt="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"
  local cube_cli_ide="/Applications/STM32CubeIDE.app/Contents/Eclipse/plugins/com.st.stm32cube.ide.mcu.externaltools.cubeprogrammer.macosaarch64_1.0.0.202601242230/tools/bin/STM32_Programmer_CLI"
  local cube_cli_resources="/Applications/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/Resources/bin/STM32_Programmer_CLI"

  local cube_cli=""
  if [[ -x "$cube_cli_macos" ]]; then
    cube_cli="$cube_cli_macos"
  elif [[ -x "$cube_cli_macos_alt" ]]; then
    cube_cli="$cube_cli_macos_alt"
  elif [[ -x "$cube_cli_ide" ]]; then
    cube_cli="$cube_cli_ide"
  elif [[ -x "$cube_cli_resources" ]]; then
    cube_cli="$cube_cli_resources"
  elif command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    cube_cli="$(command -v STM32_Programmer_CLI)"
  fi

  if [[ -n "$cube_cli" ]]; then
    echo "Flashing with stm32cubeprogrammer"
    "$cube_cli" -c port=SWD -w "$build_dir/zephyr/zephyr.hex" -v -rst
    return
  fi

  if command -v openocd >/dev/null 2>&1; then
    echo "STM32CubeProgrammer not found, fallback to openocd runner"

    echo "OpenOCD attempt 1: default settings"
    if "$WEST" flash -d "$build_dir" -r openocd; then
      return
    fi

    echo "OpenOCD attempt 2: connect-under-reset + slower SWD (1000 kHz)"
    if "$WEST" flash -d "$build_dir" -r openocd \
      --cmd-pre-init "adapter speed 1000" \
      --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst" \
      --cmd-reset-halt "reset halt"; then
      return
    fi

    echo "OpenOCD attempt 3: connect-under-reset + very slow SWD (400 kHz)"
    if "$WEST" flash -d "$build_dir" -r openocd \
      --cmd-pre-init "adapter speed 400" \
      --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst" \
      --cmd-reset-halt "reset halt"; then
      return
    fi

    echo "OpenOCD flash failed after retries." >&2
    echo "Tip: keep NRST pressed, run ./r.sh flash, and release NRST when flashing starts." >&2
    exit 1
  fi

  echo "No supported flash tool found." >&2
  echo "Install one of:" >&2
  echo "  1) STM32CubeProgrammer (provides STM32_Programmer_CLI)" >&2
  echo "  2) openocd (brew install open-ocd)" >&2
  exit 1
}

clean_build() {
  if [[ -d "$BUILD_DIR" ]]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
  else
    echo "No temporary build directory to remove: $BUILD_DIR"
  fi

  if [[ -d "$BUILD_DIR_DEMO" ]]; then
    echo "Removing $BUILD_DIR_DEMO"
    rm -rf "$BUILD_DIR_DEMO"
  else
    echo "No temporary build directory to remove: $BUILD_DIR_DEMO"
  fi

  if [[ -d "$BUILD_DIR_DEMO_PROD" ]]; then
    echo "Removing $BUILD_DIR_DEMO_PROD"
    rm -rf "$BUILD_DIR_DEMO_PROD"
  else
    echo "No temporary build directory to remove: $BUILD_DIR_DEMO_PROD"
  fi

  if [[ -d "$BUILD_DIR_PCM" ]]; then
    echo "Removing $BUILD_DIR_PCM"
    rm -rf "$BUILD_DIR_PCM"
  else
    echo "No temporary build directory to remove: $BUILD_DIR_PCM"
  fi

  if [[ -d "$BUILD_DIR_PCM_PROD" ]]; then
    echo "Removing $BUILD_DIR_PCM_PROD"
    rm -rf "$BUILD_DIR_PCM_PROD"
  else
    echo "No temporary build directory to remove: $BUILD_DIR_PCM_PROD"
  fi
}

detect_port() {
  local port

  for port in /dev/cu.usbmodem*(N) /dev/tty.usbmodem*(N) /dev/cu.usbserial*(N) /dev/tty.usbserial*(N); do
    echo "$port"
    return 0
  done

  return 1
}

monitor_serial() {
  local port

  if [[ -n "${SERIAL_PORT:-}" ]]; then
    port="$SERIAL_PORT"
  elif ! port="$(detect_port)"; then
    echo "No serial device found (expected /dev/cu.usbmodem*, /dev/tty.usbmodem*, /dev/cu.usbserial* or /dev/tty.usbserial*)" >&2
    echo "Tip: connect board and run: ls /dev/*usb*" >&2
    exit 1
  fi

  echo "Opening serial monitor on $port @ $BAUD"
  echo "Exit screen: Ctrl-A then K"
  screen "$port" "$BAUD"
}

main() {
  if [[ $# -ne 1 ]]; then
    usage
    exit 1
  fi

  case "$1" in
    build)
      build_app
      ;;
    flash)
      flash_app
      ;;
    monitor)
      monitor_serial
      ;;
    all)
      build_app
      flash_app
      monitor_serial
      ;;
    build-demo)
      build_demo_app
      ;;
    flash-demo)
      flash_app "$BUILD_DIR_DEMO"
      ;;
    all-demo)
      build_demo_app
      flash_app "$BUILD_DIR_DEMO"
      monitor_serial
      ;;
    build-demo-prod)
      build_demo_prod_app
      ;;
    flash-demo-prod)
      flash_app "$BUILD_DIR_DEMO_PROD"
      ;;
    build-prod)
      build_prod_app
      ;;
    flash-prod)
      flash_app "$BUILD_DIR"
      ;;
    build-pcm)
      build_pcm_app
      ;;
    flash-pcm)
      flash_app "$BUILD_DIR_PCM"
      ;;
    all-pcm)
      build_pcm_app
      flash_app "$BUILD_DIR_PCM"
      monitor_serial
      ;;
    build-pcm-prod)
      build_pcm_prod_app
      ;;
    flash-pcm-prod)
      flash_app "$BUILD_DIR_PCM_PROD"
      ;;
    build-pcm5102a-test)
      build_pcm5102a_test_app
      ;;
    flash-pcm5102a-test)
      flash_app "build-zephyr-f401rct6-pcm5102a-test"
      ;;
    build-uac2-macos)
      build_uac2_macos_app
      ;;
    run-uac2-macos)
      run_uac2_macos_app
      ;;
    build-uac2-stm32)
      build_uac2_stm32_app
      ;;
    flash-uac2-stm32)
      flash_app "build-zephyr-f401rct6-uac2"
      ;;
    build-uac2-stm32-prod)
      build_uac2_stm32_prod_app
      ;;
    flash-uac2-stm32-prod)
      flash_app "build-zephyr-f401rct6-uac2-prod"
      ;;
    clean)
      clean_build
      ;;
    *)
      usage
      exit 1
      ;;
  esac
}

main "$@"
