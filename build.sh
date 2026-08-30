#!/usr/bin/env bash
# Build / flash / monitor ClaudeWatch with arduino-cli.
#   ./build.sh compile      compile only
#   ./build.sh upload       flash the last build (auto-detects /dev/cu.usbmodem*)
#   ./build.sh monitor      serial monitor (Ctrl+C to quit)
#   ./build.sh ota          flash the last build over Wi-Fi (HOST=ip to override claude-watch.local)
#   ./build.sh all          compile + upload (default)
#   PORT=/dev/cu.usbmodem1101 ./build.sh upload
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$ROOT/firmware/ClaudeWatch"
LIBS="$ROOT/firmware/libraries"
BUILD="$ROOT/firmware/build"

# Waveshare-validated: arduino-esp32 3.3.10, 16MB flash layout app3M_fat9M_16MB.
# CDCOnBoot=cdc routes Serial to the native USB port (the board has no UART bridge).
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,UploadSpeed=921600"

find_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true
}

do_compile() {
  echo "==> compile ($FQBN)"
  arduino-cli compile --fqbn "$FQBN" --libraries "$LIBS" --build-path "$BUILD" --warnings none --build-property compiler.optimization_flags=-O2 "$SKETCH"
}

do_upload() {
  local port; port="$(find_port)"
  if [[ -z "$port" ]]; then
    echo "!! no /dev/cu.usbmodem* found. Plug the board in (or hold BOOT while plugging)."; exit 1
  fi
  echo "==> upload -> $port"
  arduino-cli upload --fqbn "$FQBN" --input-dir "$BUILD" -p "$port" "$SKETCH"
}

do_monitor() {
  exec python3 "$ROOT/host/watchctl.py" monitor
}

case "${1:-all}" in
  compile) do_compile ;;
  upload)  do_upload ;;
  monitor) do_monitor ;;
  ota)     echo "==> OTA -> http://${HOST:-claude-watch.local}/api/ota"; curl -s -m 180 -F "file=@$BUILD/ClaudeWatch.ino.bin" "http://${HOST:-claude-watch.local}/api/ota"; echo ;;
  all)     do_compile; do_upload ;;
  *) echo "usage: $0 [compile|upload|ota|monitor|all]"; exit 2 ;;
esac
