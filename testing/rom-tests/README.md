# ROM tests — named Cafe OS / GX2 assertions

A growable suite of independently named assertions, each reporting its own verdict:

```
TEST mem_align_4096 PASS
TEST gx2_tilemode_resolved FAIL expected=!=DEFAULT got=0
TEST something SKIP reason=needs-content
```

This is deliberately a different shape from `testing/cpu-tests/`, which runs one monolithic
routine and returns a failure count. **A named-test suite is the one that grows**: a new test is a
function and a line in the table, and the runner picks it up with no changes.

## Why expectations rather than "all must pass"

Most of what this emulator gets wrong is already known and measured. A harness that goes red on
every known defect trains everyone to ignore it. The signal worth having is **change**:

| outcome | meaning | build |
|---|---|---|
| `REGRESSED` | a test that used to pass now doesn't | **fails** |
| `MISSING` | a test in the expectation emitted no verdict at all | **fails** |
| no expectation on file | nothing to compare against | **fails** |
| `FIXED` | a test that used to fail now passes | passes, but **re-record** |
| blacklisted | known-broken, deliberately silenced | passes |

`MISSING` exists because the obvious implementation misses it. Iterating only over *actual* results
means a ROM that crashes halfway through — or a test quietly deleted — reports a clean run. That hole
was real here until a phantom entry in an expectation file caught it.

## Running

Toolchain setup is in [`testing/toolchain/`](../toolchain/README.md).

```sh
export DEVKITPRO=$HOME/.local/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

make
./run_tests.sh                          # every suite, both arms
./run_tests.sh --config interpreter     # one arm
./run_tests.sh --update                 # re-record expectations — review the diff
```

Two arms run by default. The recompiler is what ships; `--force-interpreter` is a second opinion
built from the same ISA tables, so **a test failing in only one arm localises the defect to that
execution path**. The same reasoning as `testing/cpu-tests/`.

## Adding a test

1. Write a `static void test_foo(void)` in `source/main.c` that calls `check()`, `pass()`, `fail()`
   or `skip()`.
2. Add it to the `TESTS[]` table.
3. `make && ./run_tests.sh` — it will report `NEW`, because there is no expectation yet.
4. Review the verdict, then `./run_tests.sh --update`.

**Step 3 is not a formality.** Recording an expectation without reading it is how a wrong verdict
becomes the baseline everything is measured against.

## What belongs here

Assertions that are cheap, would corrupt silently if wrong, and need no hardware. The current set
covers heap alignment (which every GX2 surface depends on), `OSBlockMove` (one of the hottest HLE
calls in BotW), clock monotonicity and advance, `OSYieldThread` (79% of guest context switches), and
`GX2CalcSurfaceSizeAndAlignment` — a pure function driving every texture allocation, where a wrong
size or an unresolved tile mode is silent memory corruption.

What does **not** belong: anything needing a reference implementation or real silicon. Instruction
semantics go to `testing/cpu-tests/` (`ppc750cl.s`, validated against real Espresso); anything
needing a known-good image needs a differential harness that does not exist yet.

## Blacklist

`blacklist.txt` silences known-broken tests. Every entry needs a reason and, where one exists, a
roadmap id from `docs/status/ledger.json`. An entry with no reason is indistinguishable from someone
silencing a test they didn't want to fix, which is how a suite stops meaning anything.
