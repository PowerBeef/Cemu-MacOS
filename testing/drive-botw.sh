#!/bin/bash
# Boot Breath of the Wild and drive it, unattended, and load the most recent save.
#
# Which scene you land in depends on the save installed at
#   ~/Library/Application Support/TesseraEmu/mlc01/usr/save/00050000/101c9400/user/80000001
# Two are useful:
#   Shrine of Resurrection - 30 fps, GPU ~14ms, the stable A/B reference
#   Korok Forest           - 20 fps, GPU ~17ms, dense foliage, the open-world case
#
#   ./testing/drive-botw.sh [seconds_to_settle]
#
# Prints the pid on success. Shared keyboard profile + button map live under
# testing/profiles/; Python helpers under testing/playtest/ (Phase 0 playtest stack).
#
# No controller and no human input required. This works because:
#   - controllerProfiles/ ships empty, so we install gamepad-keyboard.xml as controller0.xml
#   - button values are macOS virtual key codes (wx raw keycode pass-through on macOS)
#   - System Events key down/up must HOLD ≥~150ms so one emulated frame samples the key
set -euo pipefail

SETTLE="${1:-90}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CEMU_DIR="$HOME/Library/Application Support/TesseraEmu"
ROM="$REPO/Roms/Legend of Zelda, The - Breath of the Wild (USA) (En,Fr,Es).wux"
BIN="$REPO/bin/TesseraEmu_relwithdebinfo"
PROFILE="$REPO/testing/profiles/gamepad-keyboard.xml"

[ -f "$ROM" ] || { echo "missing ROM: $ROM" >&2; exit 1; }
[ -x "$BIN" ] || { echo "missing binary: $BIN (build first)" >&2; exit 1; }
[ -f "$PROFILE" ] || { echo "missing profile: $PROFILE" >&2; exit 1; }

mkdir -p "$CEMU_DIR/controllerProfiles"
cp "$PROFILE" "$CEMU_DIR/controllerProfiles/controller0.xml"

# Prefer pkill by exact basename; fall back if none
pkill -x TesseraEmu_relwithdebinfo 2>/dev/null || true
sleep 2
: > "$CEMU_DIR/log.txt"

# CEMU_EXTRA_ARGS lets a caller add flags without editing this script, e.g.
#   CEMU_EXTRA_ARGS="--telemetry out.jsonl --telemetry-label botw-shrine" ./drive-botw.sh
# shellcheck disable=SC2086
nohup "$BIN" -g "$ROM" ${CEMU_EXTRA_ARGS:-} >/dev/null 2>&1 &
sleep 100   # boot + initial shader compilation

PID=$(pgrep -x TesseraEmu_relwithdebinfo | head -1)
[ -n "$PID" ] || { echo "TesseraEmu failed to start" >&2; exit 1; }

raise() { osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $PID) to true" >/dev/null 2>&1 || true; }

# Must HOLD: Tessera samples keystate once per emulated frame (~33ms at 30fps).
press() {
  osascript -e "tell application \"System Events\" to key down \"$1\"" >/dev/null 2>&1 || true
  sleep 0.15
  osascript -e "tell application \"System Events\" to key up \"$1\"" >/dev/null 2>&1 || true
}

raise; sleep 2

# 'z' is A, 'x' is B (see testing/profiles/BUTTON_MAP.md)
press z; sleep 6
# DLC "Downloaded content" entry: B backs out; harmless otherwise
press x; sleep 4
press z; sleep 6
press z; sleep 20
press z

echo "settling ${SETTLE}s for shader/pipeline compilation..." >&2
sleep "$SETTLE"

echo "$PID"
