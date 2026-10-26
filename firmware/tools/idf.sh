#!/usr/bin/env bash
set -euo pipefail
firmware_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
project_root=$(cd "$firmware_root/.." && pwd)
mode=${1:-football}
case "$mode" in football|f1|motogp|tetris|word|mario|luigi|pacman|invaders|pong|home|bridge|scanner) ;; *) echo "Usage: $0 {football|f1|motogp|tetris|word|mario|luigi|pacman|invaders|pong|home|bridge|scanner} [idf.py actions/options]" >&2; exit 2 ;; esac
if [ $# -gt 0 ]; then shift; fi
if [ $# -eq 0 ]; then set -- build; fi
export IDF_CCACHE_ENABLE=1
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$project_root/.tools/espressif}"
export IDF_PATH="${IDF_PATH:-$project_root/.tools/esp-idf}"
[ -d "$IDF_PATH" ] && IDF_PATH="$(cd "$IDF_PATH" && pwd -P)"  # the real path, so a worktree whose .tools is a link does not make CMake warn that IDF_PATH changed
if [ ! -f "$IDF_PATH/export.sh" ]; then
  echo "ESP-IDF is missing. Run tools/setup-idf.sh first." >&2
  exit 1
fi
# IDF's exporter discovers its installed Python environment; avoid unrelated
# shell Python versions when this project has a local SDK environment.
for python_env in "$IDF_TOOLS_PATH"/python_env/idf5.5_py*_env; do
  if [ -x "$python_env/bin/python" ]; then export PATH="$python_env/bin:$PATH"; break; fi
done
source "$IDF_PATH/export.sh" >/dev/null
# IDF compares Python path strings rather than resolved executables. Repair an
# equivalent cached path through CMake itself, retaining SDK settings and build
# outputs. A genuinely different interpreter still gets IDF's normal diagnostic.
build_python=$(python -c 'import sys; print(sys.executable)')
if python - "$firmware_root/build/$mode/CMakeCache.txt" "$build_python" <<'PYTHON'
import os
from pathlib import Path
import sys
cache = Path(sys.argv[1])
if cache.is_file():
    for line in cache.read_text().splitlines():
        if line.startswith("PYTHON:"):
            cached = line.split("=", 1)[1]
            if cached != sys.argv[2] and os.path.realpath(cached) == os.path.realpath(sys.argv[2]):
                sys.exit(0)
            break
sys.exit(1)
PYTHON
then
  cmake -S "$firmware_root" -B "$firmware_root/build/$mode" \
    "-DPYTHON=$build_python" "-DULTIMATRIX_MODE=$mode"
fi
# An older build directory keeps its saved SDK config, which ignores later changes to sdkconfig.defaults. A saved config
# that is older than sdkconfig.defaults (or lacks the TLS options the updater and feeds need) is dropped and regenerated.
saved_config="$firmware_root/build/$mode/sdkconfig"
if [ -f "$saved_config" ] && { [ "$firmware_root/sdkconfig.defaults" -nt "$saved_config" ] || [ "$firmware_root/sdkconfig.bt" -nt "$saved_config" ] || ! grep -q '^CONFIG_ESP_TLS_INSECURE=y' "$saved_config"; }; then
  echo "Regenerating $saved_config: it predates sdkconfig.defaults." >&2
  rm -f "$saved_config"
fi
# app-flash writes the first app slot. After an over-the-air install (Install update, or switching faces) the clock boots
# the other slot and ignores that write, so app-flash also erases the boot selection. UM_KEEP_OTADATA=1 turns this off.
actions=()
have_erase=0
for arg in "$@"; do [ "$arg" = erase-otadata ] && have_erase=1; done
for arg in "$@"; do
  if [ "$arg" = app-flash ] && [ "$have_erase" = 0 ] && [ "${UM_KEEP_OTADATA:-0}" != 1 ]; then actions+=(erase-otadata); fi
  actions+=("$arg")
done
set -- ${actions[@]+"${actions[@]}"}
exec "$build_python" "$IDF_PATH/tools/idf.py" -C "$firmware_root" \
  -B "$firmware_root/build/$mode" "-DULTIMATRIX_MODE=$mode" "$@"
