# How the full `ppc750cl` suite went green

**Status (2026-08-07, `791556d`):** full suite with FPSCR state on is
`RESULT=PASS failures=0` on **both** the interpreter and the recompiler.
Values-only (`IGNORE_FPSCR_STATE=1`) was already green; arms stay identical.

Ledger item: `fpscr-full-suite-green`. Run recipe: `testing/cpu-tests/`.

This note is the *how*. It is deliberately long: the residual path from 928 → 0
was a stack of independent FPSCR bugs, and re-deriving which trap killed which
count is expensive.

---

## 1. What the suite is asking for

[Andrew Church's `ppc750cl.s`](https://achurch.org/cpu-tests/ppc750cl.s) is a
public-domain PowerPC 750CL conformance suite **validated by its author against
real Espresso silicon** — the Wii U CPU, not Gekko/Broadway approximations.

For every FP and paired-single op it cares about, a check helper compares:

1. **The numeric result** (FPR / PS slots).
2. **FPSCR** as read by a sequence of `mcrfs` into CR fields, then `mfcr`.

`IGNORE_FPSCR_STATE=1` rebuilds the suite so (2) is forced to pass. That splits
the work into *wrong answers* vs *wrong bookkeeping*. Values-only green means
every answer the suite can see is Espresso-correct; full green means the
exception stickies, FI, and FPRF class match as well.

### Non-goals (held for the whole campaign)

| left alone | why |
|---|---|
| `FPSCR[FR]` (fraction rounded) | Suite never checks it (documented gap). |
| `FPSCR[OE]` / `[UE]` enabled paths | Suite does not exercise them. |
| Softfloat rewrite | Host `fmadd` / `fadd` are correct enough on values; the bug was flags. |
| Cycle accuracy | Nothing here measures time. |

---

## 2. The measured path

| milestone | full suite | notes |
|---|---:|---|
| First landing | **1,030** | 354 values + 676 FPSCR; arms identical |
| Values-only closed | 928 full / **0** values | Wrong answers gone |
| FPRF classifier + frsp | 840 | |
| mad / mul / add / div + PS defer | 285 | |
| fctiw, fadds/fsubs, estimates | 189 | |
| fcmpo / fcmpu / ps_cmp* | 152 | |
| Rc CR1, PS_DIV defer, fdivs pack | 95 | |
| PS_DIV helpers + OX | 87 | |
| fctiw round-then-range, fres, tininess | 45 | |
| PS add/sub/div pack OX, UX | 31 | |
| VE mixed-lane FPRF, pack OX under suppress | 7 | |
| single-domain FPRF bits, pack-after-div, soft FI on double fma `b≈0` | 5 | all mad |
| FMA residual + TwoSum fadd (`791556d`) | **0** | both arms |

Every row above is both arms identical unless noted. Divergence would have meant
the AArch64 backend; identity meant shared helper semantics.

---

## 3. Architecture that made the campaign possible

All FP arithmetic that the recompiler and interpreter share goes through
`ATTR_MS_ABI` helpers in `PPCInterpreterFPU.cpp` (and PS wrappers). The
recompiler emits calls into those helpers so NaN selection, VE/ZE suppress, and
FPSCR commit cannot drift between arms.

### Sticky notes and commit

```
bind_dest  →  clear pending sticky, remember prior frD / VE / ZE
  helper   →  compute result; OR into s_fpscr_pending_sticky
  commit   →  take pending → FI from XX; FPRF from result; or_sticky; recompute FEX/VX
```

- **FI** is derived from **XX** for ordinary arithmetic (`set_fi = sticky & XX`).
  `fres` / `ps_res` are the deliberate FI-without-XX special case.
- **FPRF** is set from the result domain: double bits, or single bits via
  `ConvertToSingleNoFTZ` so host FZ cannot reclassify denorms as zero.
- **FX** is set when a sticky in `FPSCR_ANY_X` transitions 0→1.

### Paired-single defer

PS ops update FPSCR once for the pair, with FPRF from **ps0**. Pattern:

1. `ppc_fpscr_defer_begin()` before lane 0.
2. Each lane commits into **accumulators** (`s_fpscr_acc_*`), not guest FPSCR.
3. `ppc_fpscr_defer_end(ps0)` writes the combined sticky + ps0 FPRF.

**Trap:** `ppc_fma_bind_dest` (and lane rebinds) **clear pending**. Any OX/XX
noted with `ppc_fpscr_note_sticky` between lanes is lost. `ppc_ps_pack_arith`
therefore ORs overflow into **`s_fpscr_acc_sticky`** while deferring.

**VE whole-reg suppress:** if either lane is invalid under VE, the destination
is left unchanged; stickies still accumulate. FPRF for the abort path is taken
from the would-be ps0 when only ps1 was invalid.

### Single-domain pack

`*s` and PS arithmetic pack through `ConvertToSingleNoFTZ` (truncate-style denorm
sticky the suite expects), not always IEEE `(float)` RN. Overflow of a finite
double intermediate past `±HUGE_VALF` becomes `±Inf` with OX|XX.

---

## 4. Host traps on Apple Silicon (why residual was hard)

These are load-bearing. Re-proposing "just read FPSR.IXC" without them will re-open
counts.

### 4.1 Hardware `fmadd` can be right when IXC is clear

Suite edge (RTZ): `1.333… × 1.5 + 0` must store **2 − 1 ulp** with FI|XX.

On M-series:

- Inline AArch64 `fmadd` under FPCR RMode=RZ yields the correct 2−ulp value.
- `std::fma` may constant-fold or ignore directed rounding in some builds; the
  emulator path is compiled to `fmadd` and matches Espresso on the **value**.
- **FPSR.IXC is still clear** for that op. Host inexact is not a reliable FI source
  under RTZ halfway cases.

Exact product is `2 − 2⁻⁵³` (halfway in the binade below 2). Espresso RTZ →
2−ulp with inexact. Host flags lie.

### 4.2 RZ≠RU via `fesetround` is optimized away

```cpp
fesetround(FE_TOWARDZERO);  auto rz = fma(a,c,b);
fesetround(FE_UPWARD);      auto ru = fma(a,c,b);
if (rz != ru) set XX;
```

In RelWithDebInfo, LLVM treats `fma` as **rounding-mode independent**, CSEs the
two calls, and the compare becomes vacuous (confirmed in
`ppc_fma_double_commit` disassembly). Do not reintroduce this pattern.

### 4.3 Same-function residual can reassociate to zero

```cpp
r = fma(a, c, b);
resid = fma(a, c, -r) + b;  // algebraically 0 if fma were exact
```

If the compiler sees both in one function, it may rewrite `resid` to 0 even when
the rounded `fma` was inexact. **Mitigation:** `noinline` residual helpers so the
producer and the checker do not share an SSA web.

### 4.4 The failure record lies about *which* op failed

`bl record` snapshots the instruction word **once**. Later checks re-use that
record. The last double residual looked like `fmadd` @ `0x02008188` with
FPRF-only FPSCR, but the failing check was the **follow-up `fadd`** of
`(2−ulp) + 2⁻⁵³` under RTZ — value still 2−ulp, FI missing. Always read the
suite source around the address, not only the mnemonic.

---

## 5. Final levers (`791556d`)

### FMA inexact — residual against the rounded result

```text
t     = fma(a, c, -r)     // a*c − r in one rounding
resid = t + b             // or fma(1, t, b)
if (resid bits << 1) != 0 → XX  (hence FI at commit)
```

- Called from `ppc_fma_double_commit` on the **pre-negate** fused result
  (`fmsub` already passes `−b`).
- Single domain: same after pack, or `fmaf` residual on exact f32 operands
  (`ppc_fpscr_note_fmaf_inexact`).
- Residual against the **packed** single so trunc to single counts as inexact
  even when the double intermediate was exact.

### fadd / fsub inexact — TwoSum error term

```text
z     = r - a
resid = (a - (r - z)) + (b - z)
nonzero → XX
```

`ppc_fsub` passes `−b` into the same helper. This is what closed the last full-suite
failure after FMA residual alone left one `fmadd`-labeled record.

### Already in place before the last mile (do not rip out)

- FPRF from integer single bits; tininess-before-rounding UX when XX and
  `|exact| < min_normal`.
- Software OX when finite inputs produce Inf.
- `fres` table residual → FI without XX.
- PS defer accumulators; pack OX into acc under defer.
- VE/ZE suppress with sticky + FI when XX pending.
- `mtfsfi` / `mtfsf` / `mcrfs` / RN → host `fesetround` (values; flags still need
  software residual where host IXC lies).

---

## 6. How to re-verify

```sh
export DEVKITPRO=/opt/devkitpro
cd testing/cpu-tests
make                                    # IGNORE_FPSCR_STATE=0 by default
./run.sh                     | ./report.py -
./run.sh --force-interpreter | ./report.py -

# values-only control (must stay 0)
make clean && make EXTRA_DEFSYM=-Wa,--defsym,IGNORE_FPSCR_STATE=1
./run.sh | ./report.py -
```

Expect `RESULT=PASS failures=0` on both full-suite arms. Any non-zero full count
with values-only still 0 is FPSCR bookkeeping again — start from `report.py`'s
got-FPSCR breakdown, not from a softfloat rewrite.

---

## 7. What green does *not* mean

- Retail titles can still misbehave; the suite is instruction-level, not HLE or
  GPU.
- `FPSCR[FR]`, OE/UE-enabled paths, and cycle counts are unchecked.
- Homebrew exit still crashes the host UI after the verdict is printed (known;
  does not corrupt the result).
- CI builds the ROM and checks infrastructure; it does **not** run the emulator
  (Metal + window server). Regression gate for this number is local until that
  changes.

---

## 8. Pointers

| | |
|---|---|
| Helpers | `src/Cafe/HW/Espresso/Interpreter/PPCInterpreterFPU.cpp` |
| PS wrappers | `PPCInterpreterPS.cpp` |
| Recompiler calls | `PPCRecompilerImlGenFPU.cpp` |
| Suite + runner | `testing/cpu-tests/` |
| Strategy overview | `docs/testing/00-test-strategy.md` |
| Ledger | `fpscr-full-suite-green`, earlier `cpu-conformance` |
