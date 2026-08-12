#!/usr/bin/env zsh
set -euo pipefail

BOARD="nucleo_f401re"
BUILD_DIR="build-zephyr-f401rct6"
BAUD="115200"

usage() {
  cat <<'EOF'
Usage: ./r.sh <build|flash|monitor|all|clean>

  build    Configure and build Zephyr app
  flash    Flash firmware to board via west
  monitor  Open serial monitor at 115200 baud
  all      build + flash + monitor
  clean    Remove temporary Zephyr build directory
EOF
}

build_app() {
  west build -b "$BOARD" . --build-dir "$BUILD_DIR" --pristine
}

flash_app() {
  local cube_cli_macos="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOS/bin/STM32_Programmer_CLI"
  local cube_cli_macos_alt="/Applications/STMicroelectronics/STM32Cube/STM32CubeProgrammer/STM32CubeProgrammer.app/Contents/MacOs/bin/STM32_Programmer_CLI"

  if [[ -x "$cube_cli_macos" ]] || [[ -x "$cube_cli_macos_alt" ]] || command -v STM32_Programmer_CLI >/dev/null 2>&1; then
    echo "Flashing with stm32cubeprogrammer"
    west flash -d "$BUILD_DIR"
    return
  fi

  if command -v openocd >/dev/null 2>&1; then
    echo "STM32CubeProgrammer not found, fallback to openocd runner"

    echo "OpenOCD attempt 1: default settings"
    if west flash -d "$BUILD_DIR" -r openocd; then
      return
    fi

    echo "OpenOCD attempt 2: connect-under-reset + slower SWD (1000 kHz)"
    if west flash -d "$BUILD_DIR" -r openocd \
      --cmd-pre-init "adapter speed 1000" \
      --cmd-pre-init "reset_config srst_only srst_nogate connect_assert_srst" \
      --cmd-reset-halt "reset halt"; then
      return
    fi

    echo "OpenOCD attempt 3: connect-under-reset + very slow SWD (400 kHz)"
    if west flash -d "$BUILD_DIR" -r openocd \
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
}

detect_port() {
  local port

  for port in /dev/cu.usbmodem* /dev/cu.usbserial*; do
    if [[ -e "$port" ]]; then
      echo "$port"
      return 0
    fi
  done

  return 1
}

monitor_serial() {
  local port

  if ! port="$(detect_port)"; then
    echo "No serial device found (expected /dev/cu.usbmodem* or /dev/cu.usbserial*)" >&2
    echo "Tip: connect board and run: ls /dev/cu.usb*" >&2
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
