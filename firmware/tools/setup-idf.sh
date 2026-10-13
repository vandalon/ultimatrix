#!/usr/bin/env bash
set -euo pipefail
project_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
export IDF_TOOLS_PATH="${IDF_TOOLS_PATH:-$project_root/.tools/espressif}"
export IDF_PATH="${IDF_PATH:-$project_root/.tools/esp-idf}"
if [ ! -d "$IDF_PATH" ]; then
  mkdir -p "$(dirname "$IDF_PATH")"
  git clone --branch v5.5.5 --depth 1 --recurse-submodules --shallow-submodules     https://github.com/espressif/esp-idf.git "$IDF_PATH"
fi
if [ "$(git -C "$IDF_PATH" describe --tags --exact-match HEAD)" != "v5.5.5" ]; then
  echo "Ultimatrix requires ESP-IDF v5.5.5. Existing SDK left untouched." >&2
  exit 1
fi
"$IDF_PATH/install.sh" esp32
