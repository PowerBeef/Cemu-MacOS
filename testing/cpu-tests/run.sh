#!/usr/bin/env bash
# Run the ppc750cl conformance ROM on TesseraEmu and print the raw TESSERA-CPUTEST
# output. Pipe it through report.py to classify.
#
#   ./run.sh                                  # recompiler arm
#   ./run.sh --force-interpreter              # interpreter arm (the control)
#
# The ROM has no UI and exits on its own, but the emulator opens a window and does
# not always exit cleanly, so this bounds the run and then kills it. The telemetry
# writer is unbuffered and OSReport is forwarded synchronously, so nothing is lost.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RPX="$HERE/ppc750cl.rpx"
EMU="${EMU:-$REPO/bin/TesseraEmu_relwithdebinfo}"
TIMEOUT="${TIMEOUT:-45}"

[ -f "$RPX" ] || { echo "missing $RPX -- run 'make' first" >&2; exit 2; }
[ -x "$EMU" ] || { echo "missing emulator at $EMU (override with EMU=...)" >&2; exit 2; }

log="$(mktemp -t cputest)"
"$EMU" --game "$RPX" --forward-console-logging "$@" > "$log" 2>&1 &
pid=$!

# Stop as soon as the ROM reports its verdict, rather than always waiting TIMEOUT.
for _ in $(seq 1 "$TIMEOUT"); do
	grep -q 'TESSERA-CPUTEST end' "$log" 2>/dev/null && break
	kill -0 "$pid" 2>/dev/null || break
	sleep 1
done

kill -TERM "$pid" 2>/dev/null
sleep 1
kill -9 "$pid" 2>/dev/null
wait "$pid" 2>/dev/null

if ! grep -q 'TESSERA-CPUTEST' "$log"; then
	echo "no TESSERA-CPUTEST output -- did the ROM boot?" >&2
	tail -20 "$log" >&2
	rm -f "$log"
	exit 1
fi

cat "$log"
rm -f "$log"
