#!/usr/bin/env bash
# Build the browser flasher payload: one merged full-flash image (bootloader + partitions +
# otadata + app) and an ESP Web Tools manifest per mode, written to web-flasher/firmware/.
# Usage: build-webflasher.sh [mode ...]   (default: every mode that has been built)
# Run after building the modes (tools/idf.sh <mode> build). CI runs it for releases; the output is not committed.
set -euo pipefail
firmware_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
project_root=$(cd "$firmware_root/.." && pwd)
out="$project_root/web-flasher/firmware"
all=(home football f1 motogp tetris word mario luigi pacman invaders pong nba)
modes=("$@"); [ ${#modes[@]} -gt 0 ] || modes=("${all[@]}")

for python_env in "$project_root"/.tools/espressif/python_env/idf5.5_py*_env; do
  if [ -x "$python_env/bin/python" ]; then py="$python_env/bin/python"; break; fi
done
py=${py:-python3}

mkdir -p "$out"
for mode in "${modes[@]}"; do
  b="$firmware_root/build/$mode"
  [ -f "$b/ultimatrix.bin" ] || { echo "$mode: not built, skipped" >&2; continue; }
  version=$(head -c 112 "$b/ultimatrix.bin" | tail -c +49 | head -c 32 | tr -d '\0')
  mkdir -p "$out/$mode"
  (cd "$b" && "$py" -m esptool --chip esp32 merge_bin -o "$out/$mode/ultimatrix-$mode.bin" \
    --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x1000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
    0xe000 ota_data_initial.bin 0x10000 ultimatrix.bin) >/dev/null
  cat > "$out/$mode/manifest.json" <<JSON
{
  "name": "Ultimatrix $mode",
  "version": "$version",
  "new_install_prompt_erase": true,
  "builds": [
    { "chipFamily": "ESP32", "parts": [ { "path": "ultimatrix-$mode.bin", "offset": 0 } ] }
  ]
}
JSON
  echo "$mode: $version"
done
