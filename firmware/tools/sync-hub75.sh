#!/usr/bin/env bash
# Copies the driver sources to the public driver repo (default ~/ESP32-HUB75-DMA-Memory-Optimized) and shows what changed.
# Only src/ is copied: the repo's LICENSE.txt, README and metadata are maintained there. Commit and push in the repo yourself.
set -euo pipefail
src=$(cd "$(dirname "${BASH_SOURCE[0]}")/../vendor/hub75/src" && pwd)
dest=${1:-$HOME/ESP32-HUB75-DMA-Memory-Optimized}
[ -d "$dest/src" ] || { echo "No driver repo at $dest" >&2; exit 1; }
rsync -a "$src/" "$dest/src/"
git -C "$dest" status --short
