#!/bin/bash
# Capture a golden reference frame + runtime stats from a running Cemu instance.
#
#   ./testing/capture-scene.sh <pid> <scene-name>
#
# Writes testing/golden/<scene-name>.png and appends a line to
# testing/golden/baseline.tsv. Captures only Cemu's own window, never the
# whole screen.
set -euo pipefail
PID="${1:?usage: capture-scene.sh <pid> <scene-name>}"
NAME="${2:?usage: capture-scene.sh <pid> <scene-name>}"
OUT="$(cd "$(dirname "$0")" && pwd)/golden"
mkdir -p "$OUT"

ax() { osascript -e "tell application \"System Events\" to tell (first process whose unix id is $PID) to $1" 2>/dev/null; }

TITLE=$(ax 'get value of attribute "AXTitle" of (first window whose subrole is "AXStandardWindow")')
BOUNDS=$(ax 'get {position, size} of (first window whose subrole is "AXStandardWindow")')
X=$(echo "$BOUNDS" | cut -d, -f1 | tr -d ' '); Y=$(echo "$BOUNDS" | cut -d, -f2 | tr -d ' ')
W=$(echo "$BOUNDS" | cut -d, -f3 | tr -d ' '); H=$(echo "$BOUNDS" | cut -d, -f4 | tr -d ' ')
[ -n "$W" ] || { echo "could not read window bounds for pid $PID" >&2; exit 1; }

screencapture -x -R"${X},${Y},${W},${H}" "$OUT/${NAME}.png"

FPS=$(echo "$TITLE" | sed -n 's/.*FPS: \([0-9.]*\).*/\1/p')
API=$(echo "$TITLE" | grep -oE '\[(Metal|Vulkan|OpenGL)\]' | head -1 | tr -d '[]')
GPU=$(echo "$TITLE" | grep -oE '\[(Apple|AMD|Intel|NVIDIA) GPU\]' | head -1 | tr -d '[]')
read -r CPU RSS <<<"$(ps -p "$PID" -o %cpu=,rss=)"
THREADS=$(ps -M "$PID" | tail -n +2 | wc -l | tr -d ' ')

printf '%s\t%s\t%s\t%s\t%s\t%s MB\t%s threads\t%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$(git rev-parse --short HEAD)" "$NAME" \
  "${FPS:-?}" "${API:-?}/${GPU:-?}" "$((RSS/1024))" "$THREADS" "$(echo "$CPU" | tr -d ' ')%" \
  >> "$OUT/baseline.tsv"

echo "captured $OUT/${NAME}.png"
tail -1 "$OUT/baseline.tsv"
