#!/usr/bin/env bash
# Builds the bridge image and its md5, for clocks that still run the old firmware.
# Output: dist/bridge/bridge.bin and bridge.md5. Rename both to cw-cf-0xNN.bin / cw-cf-0xNN.md5 for each face id
# the old clocks run and put them in the folder the old clocks use as their firmware location.
set -euo pipefail
firmware_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
out="$(cd "$firmware_root/.." && pwd)/dist/bridge"
"$firmware_root/tools/idf.sh" bridge build
mkdir -p "$out"
cp "$firmware_root/build/bridge/ultimatrix.bin" "$out/bridge.bin"
md5 -q "$out/bridge.bin" > "$out/bridge.md5" 2>/dev/null || md5sum "$out/bridge.bin" | cut -d' ' -f1 > "$out/bridge.md5"
echo "bridge.bin $(wc -c < "$out/bridge.bin") bytes, md5 $(cat "$out/bridge.md5")"
