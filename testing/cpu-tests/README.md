# CPU accuracy tests — PowerPC 750CL / Espresso

The fork's first CPU conformance test. It runs Andrew Church's `ppc750cl.s` as ordinary Wii U
homebrew, so it needs **no game image, no console, no keys and no SDK**.

## Why this suite

There is no Wii U conformance suite. The only Wii U test programs that exist publicly are ~30
abandoned smoke tests in `decaf-emu/wiiu-tests`, of which exactly one touches the GPU. `ppc750cl.s`
is the exception, and it is unusually well suited to this fork:

- It exercises **every instruction the 750CL implements**, including all 29 `ps_*` and all 8 `psq_*`
  paired-single instructions — the part of the ISA that the AArch64 backend is most likely to get
  wrong and that no generic PowerPC suite covers.
- Its author validated it **against a real Espresso processor** — not Gekko, not Broadway, but the
  exact guest CPU this emulator implements. From the file header:

  > "The routine (excluding the tests of the absolute branch, trap, and system call instructions)
  > has been validated against a real Espresso (750CL compatible) processor."

- Failures come back as machine-readable 32-byte records, so this is a CI signal rather than
  something a human has to read.

## Provenance and licence

`ppc750cl.s` is fetched verbatim from <https://achurch.org/cpu-tests/ppc750cl.s> (23,502 lines,
last updated 2019-01-11). Its header states:

> "No copyright is claimed on this file."

It is committed unmodified. The suite defines no entry symbol by design — its header offers
"include this file in another file immediately after a symbol definition", which is what
`ppc750cl_entry.s` does. **Do not edit `ppc750cl.s`**; change the wrapper instead, so the file stays
byte-identical to upstream and can be re-fetched and diffed.

## Assembler settings, and why

| symbol | value | reason |
|---|---|---|
| `HAVE_UGQR` | **1** | The Espresso flag. GQRs are writable via the user-accessible UGQR SPRs 896–903, which is what makes the `psq_*` quantisation tests meaningful. The suite's own changelog notes this define was *renamed from `ESPRESSO`*. |
| `CAN_SET_GQR` | 0 | SPRs 912–919 are supervisor-only; we run in user mode under Cafe OS. |
| `TEST_SC` | 0 | `sc` traps into Cafe OS rather than returning to the suite. |
| `TEST_TRAP` | 0 | Same reasoning for `td/tdi/tw/twi`. |
| `TEST_BA` | 0 | `ba`/`bla` require the code to be loaded at exactly `0x1000000`; an RPX's load address is not ours to choose. |
| `TEST_PAIRED_SINGLE` | 1 (default) | The whole point. Requires `HID2[PSE]`, which Cafe OS sets — every retail title uses paired singles as its SIMD. |
| `IGNORE_FPSCR_STATE` | 0 | Left **off** deliberately. If FPSCR state bits dominate the output and bury real defects, turning it on is the documented escape hatch — but record the count with it off first, because that number is the honest one. |

## Building and running

Needs a devkitPPC + wut toolchain. If devkitPro's installer is unavailable, `docs/testing/00-test-strategy.md`
§4 documents building one from source without root.

```sh
export DEVKITPRO=$HOME/.local/devkitpro
export DEVKITPPC=$DEVKITPRO/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

make                                          # -> ppc750cl.rpx and .wuhb
./run.sh                     | ./report.py -  # recompiler arm
./run.sh --force-interpreter | ./report.py -  # interpreter arm

# separate FPSCR bookkeeping from genuinely wrong results
make clean && make EXTRA_DEFSYM=-Wa,--defsym,IGNORE_FPSCR_STATE=1
```

Both arms matter. The recompiler is what ships; the interpreter is a second opinion built from the
same ISA tables. **Divergence between the two arms is itself a result** — it localises a defect to
the AArch64 backend rather than to the shared decode logic.

Output is `TESSERA-CPUTEST` lines on stdout via `--forward-console-logging`. Pipe them through
`report.py` to decode the failure records.

## Reading the result

- `RESULT=PASS failures=0` — every instruction the suite covers behaves as Espresso does.
- `RESULT=FAIL failures=N` — N failing instructions, one 32-byte record each.
- `RESULT=BOOTSTRAP-FAILED` — **much worse than a large failure count.** It means one of the
  instructions the suite assumes correct is broken: `beq`/`bne cr0`, `bl`, `blr`, `fcmpu`, `mflr`,
  `mtlr`. Fix that before reading anything else.

**A non-zero count on the first run is the expected outcome, not a setback.** This is a 23,502-line
silicon-validated suite meeting a JIT that has never been conformance-tested.

### Measured, 2026-08-03

| run | failures |
|---|---|
| recompiler | 1,030 |
| interpreter | 1,030 |
| unique to either arm | **0** |
| recompiler, `IGNORE_FPSCR_STATE=1` | **354** |

**The recompiler and interpreter fail identically**, so no defect here is specific to the AArch64
backend. **676 of 1,030 are FPSCR state bits** the emulator does not maintain -- a documented
shortcut, not bugs. The remaining **354 are wrong values**: 175 paired-single, 120 double-extended,
36 single-extended, 19 `psq_*`, and only **3 integer**. The integer core is essentially correct; the
FP and paired-single paths are not.

## What it does not cover

Documented gaps, so nobody assumes more coverage than exists: `bca`/`bcla`/`eciwx`/`ecowx`; D-form
and DS-form loads/stores to absolute addresses (RA=0); FP operations with `FPSCR[OE]` or `FPSCR[UE]`
set; the effect of FP operations on `FPSCR[FR]`. Plus the three groups disabled above. **Timing and
cycle accuracy are not covered by anything and genuinely require hardware.**
