#!/usr/bin/env bash
# ESP-IDF build helper for the Brookesia (launcher) firmware with the ClaudeWatch app.
#   ./build.sh build            configure (downloads managed components on first run) + build
#   ./build.sh flash            flash the last build (auto-detects /dev/cu.usbmodem*)
#   ./build.sh monitor          idf.py monitor (Ctrl+] to quit)
#   ./build.sh all              build + flash (default)
#   ./build.sh menuconfig       interactive config
# Requires ESP-IDF at $IDF_PATH (default ~/esp/esp-idf).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf}"
# export.sh's activate_venv.py crashes (SIGABRT) on this Mac; build the environment from
# idf_tools.py directly instead (same variables export.sh would set).
IDF_PY_ENV="$(ls -d "$HOME"/.espressif/python_env/idf5.5_py*_env 2>/dev/null | head -1)"
[[ -n "$IDF_PY_ENV" ]] || { echo "ESP-IDF python env not found; run $IDF_PATH/install.sh esp32s3"; exit 1; }
ORIG_PATH="$PATH"
while IFS='=' read -r k v; do
  [[ -z "$k" ]] && continue
  [[ "$k" == "PATH" ]] && v="${v//\$PATH/$ORIG_PATH}"   # idf_tools emits a literal $PATH
  export "$k=$v"
done < <("$IDF_PY_ENV/bin/python" "$IDF_PATH/tools/idf_tools.py" export --format key-value)
export PATH="$IDF_PY_ENV/bin:$IDF_PATH/tools:$PATH:/opt/homebrew/bin"   # cmake + ninja from Homebrew

find_port() {
  if [[ -n "${PORT:-}" ]]; then echo "$PORT"; return; fi
  ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true
}

cd "$ROOT"
case "${1:-all}" in
  build)      idf.py set-target esp32s3 >/dev/null 2>&1 || true; idf.py build ;;
  flash)      idf.py -p "$(find_port)" flash ;;
  monitor)    idf.py -p "$(find_port)" monitor ;;
  menuconfig) idf.py menuconfig ;;
  all)        idf.py set-target esp32s3 >/dev/null 2>&1 || true; idf.py build && idf.py -p "$(find_port)" flash ;;
  *) echo "usage: $0 [build|flash|monitor|menuconfig|all]"; exit 2 ;;
esac
