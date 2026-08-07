#!/usr/bin/env bash
# accurateShaderMul A/B for BotW (rm-shader-cost Experiment 1).
#
# Protocol (ledger gate):
#   - equal boot/settle/record windows
#   - gameplay phase analysis via telemetry-report.py
#   - positive control: TESSERA_DUMP_MSL counts of mul_nonIEEE(
#   - do NOT use gpu.frame_critical_path_ns
#
# Usage:
#   ./testing/run-mul-ab.sh              # full: dump control + dump treatment + n=3 perf each
#   ./testing/run-mul-ab.sh dump-only
#   ./testing/run-mul-ab.sh perf-only
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$REPO/bin/TesseraEmu_relwithdebinfo"
ROM="$REPO/Roms/Legend of Zelda, The - Breath of the Wild (USA) (En,Fr,Es).wux"
GP_DIR="$HOME/Library/Application Support/TesseraEmu/gameProfiles"
PROFILE="$GP_DIR/00050000101c9400.ini"
OUT="$REPO/testing/traces/mul_ab"
PROFILE_SRC="$REPO/testing/profiles/gamepad-keyboard.xml"
CEMU_DIR="$HOME/Library/Application Support/TesseraEmu"

BOOT_S="${BOOT_S:-90}"
SETTLE_S="${SETTLE_S:-60}"
RECORD_S="${RECORD_S:-120}"
N="${N:-3}"
MODE="${1:-full}"

[ -x "$BIN" ] || { echo "missing binary" >&2; exit 1; }
[ -f "$ROM" ] || { echo "missing ROM" >&2; exit 1; }
mkdir -p "$OUT" "$GP_DIR" "$CEMU_DIR/controllerProfiles"
cp "$PROFILE_SRC" "$CEMU_DIR/controllerProfiles/controller0.xml"

write_profile() {
  local mul="$1"
  cat > "$PROFILE" <<EOF
# BotW USA — accurateShaderMul A/B harness ($(date -u +%Y-%m-%dT%H:%MZ))
[Graphics]
accurateShaderMul = $mul
EOF
  echo "profile accurateShaderMul=$mul -> $PROFILE"
}

kill_emu() {
  pkill -x TesseraEmu_relwithdebinfo 2>/dev/null || true
  sleep 2
  pkill -9 -x TesseraEmu_relwithdebinfo 2>/dev/null || true
  sleep 1
}

raise() {
  local pid="$1"
  osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $pid) to true" >/dev/null 2>&1 || true
}

press() {
  osascript -e "tell application \"System Events\" to key down \"$1\"" >/dev/null 2>&1 || true
  sleep 0.15
  osascript -e "tell application \"System Events\" to key up \"$1\"" >/dev/null 2>&1 || true
}

# BotW title -> most recent save (same as drive-botw)
botw_load_save() {
  local pid="$1"
  raise "$pid"; sleep 2
  press z; sleep 6
  press x; sleep 4
  press z; sleep 6
  press z; sleep 20
  press z
}

count_mul() {
  local dir="$1"
  python3 - <<PY
from pathlib import Path
import re
d = Path("$dir")
files = list(d.glob("*.msl"))
calls = defs = 0
for p in files:
    t = p.read_text(errors="replace")
    defs += len(re.findall(r"float\\s+mul_nonIEEE\\s*\\(", t))
    calls += len(re.findall(r"(?<!float )mul_nonIEEE\\s*\\(", t))
# also count bare mul_nonIEEE( including def — report both
raw = sum(t.count("mul_nonIEEE(") for t in (p.read_text(errors="replace") for p in files))
print(f"files={len(files)} raw_mul_nonIEEE_open_parens={raw} definitions={defs} call_sites_approx={raw - defs}")
PY
}

# One launch: optional MSL dump dir, telemetry path, label
run_once() {
  local label="$1"
  local mul="$2"
  local telem="$3"
  local dump_dir="${4:-}"

  write_profile "$mul"
  kill_emu
  : > "$CEMU_DIR/log.txt"

  local -a env_prefix=()
  if [ -n "$dump_dir" ]; then
    rm -rf "$dump_dir"
    mkdir -p "$dump_dir"
    export TESSERA_DUMP_MSL="$dump_dir"
  else
    unset TESSERA_DUMP_MSL || true
  fi

  echo "=== run $label mul=$mul telem=$(basename "$telem") dump=${dump_dir:-none} ==="
  nohup "$BIN" --verbose -g "$ROM" --telemetry "$telem" --telemetry-label "$label" \
    >"$OUT/${label}.launch.log" 2>&1 &
  sleep 3
  local pid
  pid=$(pgrep -x TesseraEmu_relwithdebinfo | head -1 || true)
  [ -n "$pid" ] || { echo "launch failed"; tail -40 "$OUT/${label}.launch.log"; return 1; }
  echo "pid=$pid boot_wait ${BOOT_S}s"
  sleep "$BOOT_S"
  pid=$(pgrep -x TesseraEmu_relwithdebinfo | head -1 || true)
  [ -n "$pid" ] || { echo "died during boot"; return 1; }

  # prove config
  if ! rg -q "Strict shader mul: ${mul}" "$CEMU_DIR/log.txt" "$OUT/${label}.launch.log" 2>/dev/null; then
    # also check launch log
    rg -n "Strict shader mul" "$OUT/${label}.launch.log" "$CEMU_DIR/log.txt" 2>/dev/null | tail -5 || true
    echo "WARN: did not find 'Strict shader mul: $mul' in logs yet (may still be loading)"
  else
    echo "OK config: Strict shader mul: $mul"
  fi

  botw_load_save "$pid"
  echo "settle ${SETTLE_S}s"
  sleep "$SETTLE_S"
  echo "record ${RECORD_S}s"
  sleep "$RECORD_S"
  kill_emu
  # wait for telemetry flush
  sleep 2
  rg -n "Strict shader mul" "$OUT/${label}.launch.log" 2>/dev/null | tail -3 || true
  if [ -n "$dump_dir" ]; then
    echo -n "MSL dump $dump_dir: "
    count_mul "$dump_dir"
  fi
  echo "frames header:"; head -1 "$telem" 2>/dev/null | python3 -c "import sys,json; h=json.loads(sys.stdin.read()); print('build',h.get('build'),'frames',h.get('frames'),'title',h.get('title_id',h.get('title')))" 2>/dev/null || echo "(no header yet)"
}

run_dump_arms() {
  run_once "mul_true_dump" true "$OUT/mul_true_dump.jsonl" "$OUT/msl_true"
  run_once "mul_false_dump" false "$OUT/mul_false_dump.jsonl" "$OUT/msl_false"
}

run_perf_arms() {
  local i
  for i in $(seq 1 "$N"); do
    run_once "mul_true_r${i}" true "$OUT/mul_true_r${i}.jsonl"
  done
  for i in $(seq 1 "$N"); do
    run_once "mul_false_r${i}" false "$OUT/mul_false_r${i}.jsonl"
  done
}

case "$MODE" in
  dump-only) run_dump_arms ;;
  perf-only) run_perf_arms ;;
  full) run_dump_arms; run_perf_arms ;;
  *) echo "usage: $0 [full|dump-only|perf-only]" >&2; exit 2 ;;
esac

echo "=== done MODE=$MODE N=$N RECORD_S=$RECORD_S ==="
echo "Analyze: python3 testing/telemetry-report.py testing/traces/mul_ab/mul_true_r1.jsonl ..."
