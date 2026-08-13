#!/usr/bin/env zsh
set -euo pipefail

BOARD="nucleo_f401re"
BUILD_DIR="build-zephyr-f401rct6"
BUILD_DIR_DEMO="build-zephyr-f401rct6-demo"
BUILD_DIR_DEMO_PROD="build-zephyr-f401rct6-demo-prod"
BUILD_DIR_PCM="build-zephyr-f401rct6-pcm5102-st7735s"
BAUD="115200"

if ! command -v west >/dev/null 2>&1 && [[ -x "${PWD:h}/.venv/bin/west" ]]; then
  path=("${PWD:h}/.venv/bin" $path)
fi
usage() {
  cat <<'EOF'
Usage: ./r.sh <build|flash|monitor|all|build-demo|flash-demo|all-demo|build-demo-prod|flash-demo-prod|build-prod|flash-prod|build-pcm|flash-pcm|all-pcm|clean>

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
  build-pcm   Build PCM5102 + ST7735S noise demo (src/main_pcm5102_st7735s.c)
  flash-pcm   Flash PCM5102 + ST7735S noise demo build
  all-pcm     build-pcm + flash-pcm + monitor
  clean    Remove temporary Zephyr build directory
EOF
}

build_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf
}

build_demo_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR_DEMO" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
}

build_demo_prod_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR_DEMO_PROD" --pristine -- -DCONF_FILE=prj_prod.conf -DMINIAUDIO_ST7735S_DEMO_MAIN=ON
}

build_prod_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR" --pristine -- -DCONF_FILE=prj_prod.conf
}

build_pcm_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR_PCM" --pristine -- -DEXTRA_CONF_FILE=prj_debug.conf -DMINIAUDIO_PCM5102_ST7735S_MAIN=ON
}

flash_app() {
  local build_dir="${1:-$BUILD_DIR}"
  local cube_cli_macos="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_Programmer_CLI"
  local cube_cli_macos_alt="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"

  if [[ -x "$cube_cli_macos" ]] || [[ -x "$cube_cli_macos_alt" ]] || command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    echo "Flashing with stm32cubeprogrammer"
    west flash -d "$build_dir"
    return
  fi

  if command -v openocd >/dev/null 2>&1; then
    echo "STM32CubeProgrammer not found, fallback to openocd runner"

    echo "OpenOCD attempt 1: default settings"
    if west flash -d "$build_dir" -r openocd; then
      return
    fi

    echo "OpenOCD attempt 2: connect-under-reset + slower SWD (1000 kHz)"
    if west flash -d "$build_dir" -r openocd \
      --cmd-pre-init "adapter speed 1000" \
      --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst" \
      --cmd-reset-halt "reset halt"; then
      return
    fi

    echo "OpenOCD attempt 3: connect-under-reset + very slow SWD (400 kHz)"
    if west flash -d "$build_dir" -r openocd \
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

  if ! port="$(detect_port)"; then
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
