#!/bin/sh
# Steps the clock through panel brightness levels with the hue-test swatch grid (needs a build with /api/hue-test; no admin PIN set).
# usage: tools/hue-test.sh HOST [SECONDS_PER_STEP] [DEEP 0|1] [LEVELS...]
# Rows: red, orange, amber, yellow, green, cyan, blue, pink; columns: value 100 70 50 35 25 18 12 8 %.
# Env WIDE=1 shows 16 hues of 4 px (red, vermilion, orange, amber, yellow, lime, green, spring, cyan, sky, blue, violet, magenta, pink, white, warm white).
# Env WG=1.5 (and WB) sets the green (blue) weights for dim colours live, 0 = default; they stay until a restart.
# Look for a swatch whose colour differs from its row (amber that looks red/pink, cyan that looks blue or white, ...) and note the level.
host=${1:?host}; secs=${2:-8}; deep=${3:-0}; shift 3 2>/dev/null
levels=${*:-"1 2 3 4 5 6 8 10 14 18 22 28 36 48 64 96 136 200"}
for b in $levels; do
  echo "brightness $b/255 ($((b * 100 / 255)) %)"
  curl -s -o /dev/null -X POST "http://$host/api/hue-test?b=$b&deep=$deep&secs=$((secs + 5))${WIDE:+&wide=1}${WG:+&wg=$WG}${WB:+&wb=$WB}"
  sleep "$secs"
done
curl -s -o /dev/null -X POST "http://$host/api/hue-test?b=0"
