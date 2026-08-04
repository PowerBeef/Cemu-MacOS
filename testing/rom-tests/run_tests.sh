#!/usr/bin/env bash
# Run the ROM test suites and diff each test's verdict against its recorded expectation.
#
#   ./run_tests.sh                 # every suite, every config
#   ./run_tests.sh --config recompiler
#   ./run_tests.sh --suite cafeos
#   ./run_tests.sh --update        # rewrite expected/ from this run (review the diff!)
#
# Shaped after PCSX2's tests/run_test.pl: for each ROM, launch the emulator with a config
# matrix, capture stdout, and compare per-test verdicts against a checked-in expectation.
#
# Why expectations rather than "all must pass": most of what this emulator gets wrong is
# already known and measured. A suite that fails 354 assertions is not a broken suite, it is
# an accurate one, and a harness that goes red on all of them teaches everyone to ignore it.
# The signal worth having is CHANGE -- a test that regressed, or one that started passing.
#
# blacklist.txt holds tests that are known-broken and deliberately not reported as failures.
# Every entry must carry a reason and, where one exists, a roadmap id.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
EMU="${EMU:-$REPO/bin/TesseraEmu_relwithdebinfo}"
TIMEOUT="${TIMEOUT:-90}"
EXPECTED_DIR="$HERE/expected"
BLACKLIST="$HERE/blacklist.txt"

only_config=""; only_suite=""; update=0
while [ $# -gt 0 ]; do
	case "$1" in
		--config) only_config="$2"; shift 2 ;;
		--suite)  only_suite="$2";  shift 2 ;;
		--update) update=1; shift ;;
		-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

[ -x "$EMU" ] || { echo "missing emulator at $EMU (override with EMU=...)" >&2; exit 2; }
mkdir -p "$EXPECTED_DIR"

# suite | rom path | marker prefix
SUITES="
cafeos|$HERE/rom_tests.rpx|TESSERA-ROMTEST
"
# config | extra emulator flags. The interpreter arm is what localises a defect to the
# AArch64 backend rather than to shared decode -- see testing/cpu-tests/README.md.
CONFIGS="
recompiler|
interpreter|--force-interpreter
"

blacklisted() {
	[ -f "$BLACKLIST" ] || return 1
	grep -qE "^[[:space:]]*$1[[:space:]]*(#|$)" "$BLACKLIST"
}

total_new=0; total_regressed=0; total_fixed=0; total_bl=0; ran=0

while IFS='|' read -r suite rom marker; do
	[ -n "${suite:-}" ] || continue
	[ -n "$only_suite" ] && [ "$only_suite" != "$suite" ] && continue
	if [ ! -f "$rom" ]; then
		echo "SKIP suite=$suite -- $rom not built (run make)" >&2
		continue
	fi

	while IFS='|' read -r cfg flags; do
		[ -n "${cfg:-}" ] || continue
		[ -n "$only_config" ] && [ "$only_config" != "$cfg" ] && continue
		ran=$((ran + 1))

		log="$(mktemp -t romtest)"
		# shellcheck disable=SC2086 -- flags must word-split
		"$EMU" --game "$rom" --forward-console-logging $flags > "$log" 2>&1 &
		pid=$!
		for _ in $(seq 1 "$TIMEOUT"); do
			grep -q "$marker end" "$log" 2>/dev/null && break
			kill -0 "$pid" 2>/dev/null || break
			sleep 1
		done
		kill -TERM "$pid" 2>/dev/null; sleep 1; kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null

		# "name VERDICT" per line, sorted so the comparison is order-independent -- test
		# order is not a contract and should not cause a spurious diff.
		actual="$(grep -oE '^TEST [A-Za-z0-9_]+ (PASS|FAIL|SKIP)' "$log" \
		          | awk '{print $2, $3}' | sort || true)"

		if [ -z "$actual" ]; then
			echo "FAIL suite=$suite config=$cfg -- no TEST lines; did the ROM boot?"
			tail -15 "$log" >&2
			rm -f "$log"; total_new=$((total_new + 1)); continue
		fi

		exp="$EXPECTED_DIR/$suite.$cfg.expected"
		if [ "$update" = 1 ]; then
			printf '%s\n' "$actual" > "$exp"
			echo "updated $exp ($(printf '%s\n' "$actual" | wc -l | tr -d ' ') tests)"
			rm -f "$log"; continue
		fi
		if [ ! -f "$exp" ]; then
			echo "NEW  suite=$suite config=$cfg -- no expectation on file."
			echo "     review, then record with: $0 --suite $suite --config $cfg --update"
			printf '%s\n' "$actual" | sed 's/^/       /'
			rm -f "$log"; total_new=$((total_new + 1)); continue
		fi

		regressed=0; fixed=0; bl=0
		while read -r name verdict; do
			[ -n "${name:-}" ] || continue
			was="$(awk -v n="$name" '$1==n {print $2}' "$exp")"
			[ "$was" = "$verdict" ] && continue
			if blacklisted "$name"; then
				bl=$((bl + 1)); continue
			fi
			if [ "$verdict" = "PASS" ]; then
				echo "FIXED     $suite/$cfg $name: $was -> PASS"
				fixed=$((fixed + 1))
			else
				echo "REGRESSED $suite/$cfg $name: ${was:-<new>} -> $verdict"
				regressed=$((regressed + 1))
			fi
		done <<< "$actual"

		# A test that stops being emitted at all must count as a regression. Iterating only
		# over actual results misses it entirely -- a ROM that crashes halfway through, or a
		# test quietly deleted, would report a clean run. Found by adding a phantom entry to
		# an expectation file and watching this report nothing.
		missing=0
		while read -r name _; do
			[ -n "${name:-}" ] || continue
			if ! printf '%s\n' "$actual" | awk -v n="$name" '$1==n {found=1} END {exit !found}'; then
				if blacklisted "$name"; then
					bl=$((bl + 1))
				else
					echo "MISSING   $suite/$cfg $name: expected a verdict, got none"
					missing=$((missing + 1))
				fi
			fi
		done < "$exp"
		regressed=$((regressed + missing))

		n=$(printf '%s\n' "$actual" | wc -l | tr -d ' ')
		echo "suite=$suite config=$cfg tests=$n regressed=$regressed fixed=$fixed blacklisted=$bl"
		total_regressed=$((total_regressed + regressed))
		total_fixed=$((total_fixed + fixed)); total_bl=$((total_bl + bl))
		rm -f "$log"
	done <<< "$CONFIGS"
done <<< "$SUITES"

echo
echo "ran $ran configuration(s): regressed=$total_regressed fixed=$total_fixed blacklisted=$total_bl new=$total_new"
[ "$total_fixed" -gt 0 ] && echo "note: a FIXED test means the expectation is now pessimistic -- re-record it."
# Only a regression or a missing expectation fails. A newly passing test is good news and
# must not break the build, but it does need the expectation re-recorded.
[ "$total_regressed" -eq 0 ] && [ "$total_new" -eq 0 ]
