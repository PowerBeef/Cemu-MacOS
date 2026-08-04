#!/usr/bin/env bash
# Run the self-dependency ROM and print the acc.self_dep_* readings.
#
#   ./run.sh              # default 40s of rendering
#   TIMEOUT=90 ./run.sh
#
# The ROM renders until the process is stopped -- there is no frame cap, because
# the point is to accumulate counter samples. This bounds the run and stops it.
#
# Killing is safe: the telemetry writer is a dedicated thread doing raw write() on
# an fd, so at most the in-flight queue is lost. That is NOT true of the emulator's
# log.txt or shader cache, which need a clean exit -- but this test reads neither.
#
# Why the read-out is inline rather than testing/telemetry-report.py: that tool
# prints "per-frame counters (median, non-zero only)" and skips any counter whose
# total is zero. The entire published result here is that three counters read ZERO,
# so the standard tool cannot show it. Zeros are still present in the per-frame
# "v" arrays; this reads them directly.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RPX="$HERE/self_dependency.rpx"
EMU="${EMU:-$REPO/bin/TesseraEmu_relwithdebinfo}"
TIMEOUT="${TIMEOUT:-40}"

[ -f "$RPX" ] || { echo "missing $RPX -- run 'make' first" >&2; exit 2; }
[ -x "$EMU" ] || { echo "missing emulator at $EMU (override with EMU=...)" >&2; exit 2; }

CL="$HOME/Library/Application Support/TesseraEmu/cafeLibs/glslcompiler.rpl"
[ -f "$CL" ] || echo "warning: $CL absent -- the ROM will report glslcompiler.rpl-not-found" >&2

log="$(mktemp -t gfxtest)"; tel="$(mktemp -t gfxtel)"
"$EMU" --game "$RPX" --forward-console-logging \
       --telemetry "$tel" --telemetry-areas accuracy,gpu "$@" > "$log" 2>&1 &
pid=$!
for _ in $(seq 1 "$TIMEOUT"); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
kill -TERM "$pid" 2>/dev/null; sleep 1; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

echo "=== ROM output ==="
grep 'TESSERA-GFXTEST' "$log" || { echo "none -- did the ROM boot?"; tail -20 "$log"; }

echo
echo "=== counters (zeros shown deliberately: a zero here IS the current result) ==="
python3 - "$tel" <<'PY'
import json, statistics, sys, pathlib
p = pathlib.Path(sys.argv[1])
if not p.exists() or not p.stat().st_size:
    print("  no telemetry written"); raise SystemExit(1)
lines = p.read_text().splitlines()
hdr = json.loads(lines[0])
if "counters" not in hdr:
    print("  header has no counter list; run ended before the first frame")
    print(" ", lines[0][:200]); raise SystemExit(1)
idx = {c["n"]: i for i, c in enumerate(hdr["counters"])}
frames = [json.loads(l) for l in lines if l.startswith('{"t":"f"')]
print(f"  frames: {len(frames)}")
for n in ("acc.self_dep_fbfetch", "acc.render_self_dependency", "acc.self_dep_nonpixel",
          "gpu.draw_calls", "gpu.render_passes"):
    if n not in idx:
        print(f"  {n:<30} NOT IN BUILD"); continue
    v = [f["v"][idx[n]] for f in frames] or [0]
    print(f"  {n:<30} median/frame {statistics.median(v):>7.1f}   total {sum(v):>10,}")
print()
print("  Expected once this reproduces: exactly ONE of self_dep_fbfetch (covered by")
print("  framebuffer fetch) or render_self_dependency (not covered) is non-zero.")
print("  Both zero means no alias was seen -- see README.md, it is unresolved.")
PY
rm -f "$log" "$tel"
