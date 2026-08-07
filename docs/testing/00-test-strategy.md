# Testing TesseraEmu — strategy, tools, provenance

**Status: the CPU conformance suite is the live gate for FP/PS accuracy.** Values-only is green;
full suite residual is FPSCR bookkeeping. Read [§7 Status](#7-status-honest) and
`docs/status` item `rm-fp-conformance` before quoting numbers.

### Current result (2026-08-07 — `rm-fp-conformance`)

| run | failures |
|---|---|
| recompiler, full FPSCR (`IGNORE` off) | **928** |
| interpreter, full FPSCR | **928** |
| failures unique to either arm | **0** |
| either arm, `IGNORE_FPSCR_STATE=1` (values only) | **0** |

**What that means:**

1. **Values-only is closed.** Every wrong *result* the suite can see with FPSCR state checks
   suppressed is fixed; arms stay identical. The integer core was already essentially correct; the
   FP and paired-single *value* paths are now suite-green under IGNORE.
2. **Full suite residual is FPSCR bookkeeping** (FPRF, FI/FR, exception stickies/enables), not a
   second pile of wrong answers. Highest volume: `frsp`, the mad family, `fctiw`, PS arithmetic.
3. **Arms remain identical** on both builds — still shared decode/semantics, not AArch64-only.

Landed along the way (ledger has the commits): PS quantize / VE / FMA, frsp/fctiw FZ paths, mad
double-rounding, FPSCR moves (mcrfs/mtfs*/CR1), mcrxr/stwcx, fdiv tininess, excess-range merge
slot asymmetry, frsqrte denorms, lfd→ps1 hazard, denorm-merge sticky store encoding. Detail lives
in `docs/status/ledger.json` and the commit trail claimed by `rm-fp-conformance`.

**Next lever:** wire FPRF + FI/FR on `frsp` and the mad family; re-measure the **full** suite.

### First result (2026-08-03, historical)

| run | failures |
|---|---|
| recompiler | **1,030** |
| interpreter | **1,030** |
| unique to either arm | **0** |
| `IGNORE_FPSCR_STATE=1` | **354** |

At first landing the arms already matched (shared semantics, not backend). Split at the time:
**676** FPSCR state, **354** wrong values — almost all FP/PS; only **3** integer. The mad family
was ~37% of the value pile and pointed at Espresso 25-bit `frC` product rounding (suite changelog:
*"fmadds/ps_madd is not rounded twice"*). That work is landed; do not re-open the value pile without
a new values-only regression.

---

## 1. The problem

The fork has no test suite, no differential correctness harness, and no automated regression of any
kind. `testing/` holds five tracked files — two shell scripts, a Python reporter, a TSV of
measurements, and a `.gitignore`. CI builds, signs and smoke-launches the binary; it asserts nothing
about correctness.

That gap is the stated gate on two roadmap CPU items (`rm-cycle-counting`, `rm-fma`) and it is why
the self-dependency detector landed unrun. It is the largest structural weakness in the project.

Two constraints shape every decision below:

- **No Wii U hardware.** Timing and cycle accuracy cannot be verified. Accept it and stop looking.
- **No retail game images.** `Roms/` is empty. Everything here must work from legally free material.

## 2. What exists in the world (research summary)

Searched exhaustively; the findings are mostly negative and that is itself the result.

| | verdict |
|---|---|
| Wii U conformance suite | **Does not exist.** |
| GX2 API conformance tests | **Does not exist** — one draw-primitive smoke test, in a repo abandoned in 2022. |
| Latte shader-decompiler test harness | **Does not exist.** Nothing anywhere takes Latte bytecode as input. |
| Tests in upstream Cemu | **None.** Build-only CI; the one `*test*` file is a GX2 addrlib bring-up harness. |
| GPU command-stream capture/replay (a Wii U FIFO player) | **Does not exist.** Dolphin has it for GC/Wii, Xenia for X360; Wii U has nothing. |
| Wii U port of the 240p Test Suite | **Does not exist** (the Wii build is useless — we do not emulate vWii). |
| AArch64 JIT emitter encoding tests | **Do not exist in any emulator**, not even Dolphin. |
| PowerPC 750CL conformance suite | **Exists, and is excellent** — see §3.1. |

piglit, VK-GL-CTS, dEQP and Amber all test a *driver's* conformance to GL/Vulkan. That is the wrong
layer for a Latte→MSL decompiler. They are useful as **test designs**, not as test suites.

## 3. The suite we are building

Layered from the guest CPU outward. Each layer states what it can and cannot prove.

### 3.1 CPU conformance — `ppc750cl.s` — **assembled, never run**

The single highest-value artefact found. See `testing/cpu-tests/README.md` for the full write-up.

- 23,502 lines of self-checking PowerPC assembly by Andrew Church.
- **Public domain** — the header states "No copyright is claimed on this file."
- **Validated by its author against a real Espresso processor.** Not Gekko, not Broadway — the exact
  guest CPU this emulator implements.
- Covers every 750CL instruction including all 29 `ps_*` and all 8 `psq_*` paired-single ops. Our
  build contains **1,105 `ps_*` and 93 `psq_*` instructions across 19,360 total**.
- Failures are 32-byte machine-readable records, so this is a CI signal, not a human read.

**Two arms, and the difference is the point.** Run under the recompiler and again under
`--force-interpreter`. A failure in both is a shared decode/semantics defect; a failure in only the
recompiler arm localises to the AArch64 backend. `testing/cpu-tests/report.py --compare` asserts on
exactly that.

**What it cannot prove:** timing, cycle counts, `bca`/`bcla`/`eciwx`/`ecowx`, absolute-address
D-form/DS-form loads and stores, FP with `FPSCR[OE]`/`[UE]` set, or `FPSCR[FR]` effects.

### 3.2 CPU differential fuzzer — **not built**

Interpreter versus recompiler on randomly generated single instructions, after decaf-emu's
`fuzz-compare` design (reimplement, do not copy — decaf is GPL-3.0 and this fork is MPL-2.0).

The cheapest idea in it is worth having on its own: a **handler-parity assertion** that
`interpreter.hasInstruction(id) ^ jit.hasInstruction(id) == 0` for every instruction in the ISA
table. That is a static check, costs nothing, and catches a whole class of "the JIT silently does
nothing" defects.

Decaf's version explicitly refuses to fuzz `psq_*` because the GQRs need configuring first. That gap
is theirs, not a law of nature — seeding GQR0–7 with legal `(type, scale)` pairs closes it, and
paired-singles are exactly where this fork is most likely to be wrong.

### 3.3 HLE / `coreinit` — **not built**

~22 adaptable tests in `decaf-emu/wiiu-tests` (GPL-3.0): alarms, coroutines, message queues, heaps,
thread cancel, filesystem read. Small, but it is the only Cafe OS test material in existence. Keep it
in a separate repo to avoid licence entanglement.

### 3.4 Graphics — self-dependency reproducer — **built, runs, does not yet reproduce**

The targeted test for the defect the last audit found. Design borrowed from piglit's
`blending-in-shader-arb.c` (MIT), whose key trick is an **integer render target so comparison is
exact with no float tolerance**.

- N sequential draws into one attachment; each fragment reads the destination and writes
  `f(dst, src)` for an invertible integer `f`. Expected buffer computed on the CPU. Assert bit-exact.
- **Case A — same-texel feedback.** Metal expresses this natively with programmable blending
  (`[[color(0)]]`, optionally `[[raster_order_group(0)]]`), reading the destination from tile memory
  with no barrier and no flush.
- **Case B — different-texel feedback** (a blur, an offset tap). Programmable blending *cannot*
  express it. This needs a copy or an encoder split.

**Why that split matters:** the audit found the MSL emitter substitutes a **coordinate-free**
framebuffer fetch — valid for Case A, wrong for Case B — and currently applies it to both. The test
must therefore cover both, and is expected to fail Case B. That failure is the documented defect.

Run it under the `acc.render_self_dependency` / `acc.self_dep_fbfetch` / `acc.self_dep_nonpixel`
counters landed alongside the detector.

**Current state: the ROM builds and runs, and all three counters read zero.** Both sides of the
covered/uncovered split being zero means the emulator saw no alias at all, which is either a test
that does not create the aliasing the emulator recognises or a detector that does not fire. Not yet
separated — see `testing/graphics-tests/README.md`. **Until it is, a zero from these counters is not
evidence about item 3.1.**

Getting this far did establish the toolchain path it depends on: CafeGLSL's `glslcompiler.rpl` loads
from `cafeLibs/` and compiles GLSL to Latte bytecode at runtime *inside* the emulator, which our
decompiler then lowers to MSL (`gpu.shaders_compiled_vs`/`ps` both incremented). That means graphics
tests can be written in ordinary GLSL and still exercise the real Latte→MSL path.

### 3.5 Golden frames — **exists, and is weak**

`testing/capture-scene.sh` captures a window-only PNG and appends to `testing/golden/baseline.tsv`.
Two structural weaknesses, both worth stating plainly:

1. **The PNGs are gitignored**, so comparison is intra-session only. There is no cross-commit image
   regression, and any ledger entry implying otherwise is wrong.
2. **~22% of pixels differ between any two captures** of an animated title screen. No perceptual
   metric survives that; mature projects tolerate 0.05–0.5%.

The fix is not a better differ, it is removing the variance at the source. Dolphin's FifoCI gets away
with plain SHA-1 of raw pixels because a FIFO log is a frozen GPU command stream with no game logic,
timers, RNG or input. A hand-written test ROM has the same property by construction.

### 3.6 Accuracy counters — **exists and works**

13 `acc.*` telemetry counters. On BotW Korok gameplay, every wired one reads exactly zero across
8,881 frames except `acc.unsupported_hle_calls` (44/frame). That is a real positive result.

**Caveat that must travel with it:** `acc.render_self_dependency` had *no increment site at all*
until recently, so its zero meant nothing. Hence the standing rule — a counter reading zero is not
evidence until you have checked it has an increment site.

### 3.7 Deliberately not built

- **GX2 capture/replay** via `_GX2DebugSetCaptureInterface`. Nintendo published the hook (it is in
  wut's `gx2/debug.h`, zlib-licensed, with a `submitToRing` callback that is exactly the right
  interception point) and nobody has ever implemented it for Wii U. It is the highest-ceiling item
  available — deterministic GPU replay would make bit-exact frame hashing possible — and it is weeks
  of work. Not now.
- **Espresso timing/cycle verification.** Requires hardware. Will not happen.

## 4. The toolchain

The only supported install is the official devkitPro path into `/opt/devkitpro` — same as CI. See
[`testing/toolchain/README.md`](../../testing/toolchain/README.md):

```sh
curl -fsSLO https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg
sudo installer -pkg devkitpro-pacman-installer.pkg -target /
sudo dkp-pacman -Syu --noconfirm
sudo dkp-pacman -S --noconfirm wiiu-dev
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
```

`wiiu-dev` brings the compiler, wut-tools and wut. Graphics tests also need CafeGLSL's
`glslcompiler.rpl` (documented in the same README).

A from-source fallback once lived at `testing/toolchain/build-devkitppc.sh` for when the installer
needed interactive `sudo` and `downloads.devkitpro.org` was Cloudflare-403 from this network. Both
halves of that problem are gone on the machine of record, the official packages install cleanly, and
the fallback — with its deliberate deviations from a stock install — has been removed so there is
only one path to keep honest.

## 5. Provenance and licences

Non-negotiable for anything entering this repo.

| artefact | licence | vendored? |
|---|---|---|
| `ppc750cl.s` | **Public domain** ("No copyright is claimed on this file") | Yes, byte-identical to upstream |
| wut and its samples | zlib | No — toolchain only |
| Crementif GX2 shader examples | Unlicense (public domain) | Planned |
| piglit test *design* | MIT | Design only, not code |
| decaf-emu tests and fuzzer design | GPL-3.0 | **Never vendored** — MPL-2.0 fork; design reimplemented |
| libbinrec test catalogue | GPL-3.0 | Read as a specification only |
| Freedoom, Anarch, EasyRPG, etc. | free/libre, CC0, GPL | Not vendored; fetched by the runner |

**Excluded on legal grounds, permanently:** the leaked Nintendo Cafe SDK and anything derived from
it; reVC/re3 (DMCA'd); ports requiring purchased assets (Sonic RSDK, SpaceCadetPinball, Undertale,
Balatro); Quake/Quake II shareware `pak0.pak` (shareware terms, not an open licence); every official
Nintendo title — the Wii U eShop closed 27 March 2023 and there is no legal path to any of it.

**Rule for new material:** if a symbol name, struct offset or source file appears only in a gist or
forum post and not in wut, WiiUBrew or decaf, assume leaked-SDK origin and do not use it.

## 6. Running the tests

```sh
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"

cd testing/cpu-tests && make                               # build ppc750cl
./run.sh                     | ./report.py -               # recompiler arm
./run.sh --force-interpreter | ./report.py -               # interpreter arm
./report.py --compare recompiler.log interpreter.log       # localise defects to the backend
```

Expect `report.py` to exit non-zero. **A non-zero failure count on the first run is the expected
outcome, not a setback** — this is a silicon-validated suite meeting a JIT that has never been
conformance-tested. Every failure is a finding and belongs in `docs/status/ledger.json`.

## 7. Status (honest)

| item | status |
|---|---|
| devkitPPC + wut (`dkp-pacman -S wiiu-dev` → `/opt/devkitpro`) | **official install only** — see `testing/toolchain/README.md` |
| wut-tools (`elf2rpl`, `rplimportgen`, `wuhbtool`, …) | **via `wiiu-dev`** |
| wut (`libwut.a` + headers) | **via `wiiu-dev`** |
| `ppc750cl.rpx` / `.wuhb` | **builds** — 1,105 `ps_*`, 93 `psq_*` preserved into the ROM |
| **`ppc750cl` executed on TesseraEmu** | **YES** — values-only **0**; full FPSCR **928**; both arms identical |
| `report.py` classification and `--compare` | **working against real logs** |
| Interpreter-vs-JIT fuzzer | not built |
| HLE / `coreinit` tests | not built |
| Self-dependency reproducer | **built and runs — but the counters read zero**, see `testing/graphics-tests/README.md` |
| CI integration | **build gate only** (`.github/workflows/cpu_tests.yml`) — see below |

### What CI does and does not check

`.github/workflows/cpu_tests.yml` runs on `macos-26`, installs devkitPPC + wut the same way as
§4, builds the ROM, and asserts that:

- `vendor/ppc750cl.s` still carries its licence and validation lines and is still 23,502 lines, so
  the suite cannot silently stop being the upstream-validated one;
- the built ROM still contains ≥1000 `ps_*` and ≥80 `psq_*` instructions and exports
  `ppc750cl_test`. **A ROM that assembles without paired-single support would build cleanly and test
  nothing** — this is the check that catches that;
- `report.py` still classifies a known log.

**It does not run the ROM.** That needs the emulator binary (a ~22 minute build) plus a window server
for the Metal backend, and a job that rebuilds the emulator merely to launch a ROM would duplicate
`build_check` for a weaker signal. Running remains a local step. This is a real limitation, not an
oversight: **there is currently no automated gate on FP conformance regressions**, only on the test
infrastructure that measures them.

### Known issues

**Every homebrew title that exits normally crashes the emulator on the way out.** The guest
requesting exit reaches `MainWindow::OnRequestGameExit` → `Close()` →
`wxTopLevelWindowMac::Destroy()` → `wxWindow::Show(false)`, which faults in `objc_msgSend` on an
`NSView`. It is reproducible on every run and is newly visible only because nothing had ever run
homebrew on this fork before.

It does **not** affect results — the crash is strictly after the ROM's final output, so the verdict
is always complete — but it makes the process exit code meaningless, which is why `run.sh` asserts on
the log text rather than on `$?`.

Two hypotheses were tested and refuted; do not retry them:

1. *The canvas was destroyed while the Latte thread was still presenting to it.* Adding
   `CafeSystem::ShutdownTitle()` + `DestroyCanvas()` before `Close()` (matching what the non-CLI
   branch does via `EndEmulation()`) changed nothing.
2. *Closing a top-level window from inside an event dispatched to it.* Deferring with `CallAfter`
   changed nothing.

Both produced a byte-identical backtrace, which places the fault inside wx's own top-level teardown
rather than in our shutdown ordering. Tracked as `rm-homebrew-exit-crash`.

