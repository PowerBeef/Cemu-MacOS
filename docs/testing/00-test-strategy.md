# Testing TesseraEmu — strategy, tools, provenance

**Status: the CPU layer is built, runs, and has produced its first result. Everything else is still
plan.** Read [§7 Status](#7-status-honest) before quoting anything here as a capability.

`CLAUDE.md` says "There is **no test suite.**" That is now out of date for the CPU layer.

### First result (2026-08-03)

`ppc750cl.rpx` runs on TesseraEmu and reports machine-readable failures. Headline numbers:

| run | failures |
|---|---|
| recompiler | **1,030** |
| interpreter (`--force-interpreter`) | **1,030** |
| failures unique to either arm | **0** |
| recompiler, `IGNORE_FPSCR_STATE=1` | **354** |

Three conclusions, each of which is the point of having done this:

1. **The AArch64 recompiler is exactly as correct as the interpreter on this suite.** Not one failure
   is unique to either arm. Whatever is wrong is in shared decode/semantics, not in the JIT backend.
   That is a genuinely reassuring result about the riskiest part of the fork, and nothing before this
   could have established it.
2. **676 of the 1,030 (66%) are FPSCR state-bit bookkeeping**, which this emulator does not maintain.
   That is a deliberate, documented emulator shortcut, not 676 bugs.
3. **354 are real** — wrong values, not wrong status bits. They are almost entirely floating point:
   175 paired-single, 120 double-extended, 36 single-extended, 19 `psq_*`, and only **3 integer**.
   **The integer core is essentially correct; the FP and paired-single paths are not.**

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

### 3.4 Graphics — self-dependency reproducer — **not built**

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

## 4. The toolchain, and why it was hard

Worth recording because it is non-obvious and will be re-encountered.

**Nothing here required root.** The devkitPro installer needs `sudo`, which is unavailable when
driving this repo remotely (and the agent shell has no TTY, so sudo credential caching cannot carry
across either). Everything below builds into `$HOME`.

**`downloads.devkitpro.org` is Cloudflare-403 from this network** — including its root, while
`cloudflare.com` and `github.com` return 200, so it is site-specific rather than a general block. It
serves **every** source devkitPro's buildscripts need, not just their own components.

The workaround: devkitPro's download loop skips files that already exist
(`if [ ! -f $archive ]`) and honours `BUILD_DKPRO_SRCDIR`. Pre-staging all six archives from
reachable upstream mirrors makes the stock scripts run untouched apart from the install prefix.

| component | version | reachable source |
|---|---|---|
| binutils | 2.45.1 | ftp.gnu.org |
| gcc | 15.2.0 | ftp.gnu.org |
| newlib | 4.6.0.20260123 | sourceware.org |
| devkitppc-crtls | 1.0.0 | GitHub tag (repacked — GitHub uses `<org>-<repo>-<hash>/`, the scripts expect `<name>-<version>/`) |
| devkitppc-rules | 1.2.1 | GitHub tag (same repack) |
| binutils (mn10200) | 2.24 | **skipped** — GameCube/Wii DSP toolchain, irrelevant to Wii U |

### Four defects hit on the way, all recorded so they are not re-diagnosed

1. **Bundled zlib versus the macOS SDK.** binutils and gcc bundle a zlib whose `zutil.h` does
   `#define fdopen(fd,mode) NULL`, which then breaks the SDK's `stdio.h` declaration of `fdopen`.
   Fix: `--with-system-zlib`. devkitPro's scripts do not pass it.
2. **A genuine bug in devkitPro's buildscripts.** `extract_and_patch binutils $MN_BINUTILS_VER bz2`
   passes three arguments to a four-argument function (`name ver pkgrel ext`), so `bz2` is read as
   the package release and the extension ends up empty, producing a malformed `tar` invocation. Only
   affects the mn10200 step, which we skip.
3. **`libgloss/libsysbase/dummy.c` is missing.** The devkitPro patch adds it to the Makefile but no
   patch *creates* it — it ships inside devkitPro's repackaged newlib tarball, which is unreachable.
   54 sibling `libsysbase/*.c` files are present; only this one is absent. Stubbed as a placeholder
   translation unit. **This is the one substitution that could differ from a stock devkitPro
   install**, so it is called out rather than buried.
4. **Stock binutils is not devkitPro's binutils.** devkitPro patches `opcodes/ppc-opc.c` among
   others. A stock build assembles `ppc750cl.s` correctly (verified), but do not assume equivalence
   for RPX linking.

**A standalone stock binutils 2.45 also lives in `~/.local/ppc-binutils`.** It is what proved
`ppc750cl.s` assembles, and it is sufficient for the CPU tests alone.

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
export PATH="$HOME/.local/devkitpro/devkitPPC/bin:$PATH"   # or ~/.local/ppc-binutils/bin for CPU only

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
| devkitPPC toolchain (binutils 2.45.1, gcc 15.2.0, newlib) | **built from source**, `~/.local/devkitpro`, no root |
| wut-tools (`elf2rpl`, `rplimportgen`, `wuhbtool`, …) | **built and installed** |
| wut (`libwut.a` + headers) | **built and installed** |
| `ppc750cl.rpx` / `.wuhb` | **builds** — 1,105 `ps_*`, 93 `psq_*` preserved into the ROM |
| **`ppc750cl` executed on TesseraEmu** | **YES — 1,030 failures, both arms identical, 354 real** |
| `report.py` classification and `--compare` | **working against real logs** |
| Interpreter-vs-JIT fuzzer | not built |
| HLE / `coreinit` tests | not built |
| Self-dependency reproducer | not built |
| CI integration | not built — the toolchain is not available to CI |

### Known deviations from a stock devkitPro install

The toolchain was built from upstream sources because devkitPro's package host is unreachable here.
Three substitutions could in principle differ from an official install, and are listed so any strange
result can be checked against them first:

1. **`libgloss/libsysbase/dummy.c` was stubbed.** No devkitPro patch creates it; it ships in their
   repackaged newlib. 54 sibling files were present, only this one absent.
2. **`uint32_t`/`int32_t` are `long`/`unsigned long`** with this newlib rather than `int`/`unsigned
   int`. This forced a one-line signature fix in wut (`__syscall_lock_try_acquire_recursive`, to
   match newlib's `sys/iosupport.h`) and `-Wno-format` for wut's build. Both types are 32-bit on
   powerpc-eabi, so this is diagnostic-only, not an ABI difference.
3. **mn10200 binutils skipped** — GameCube/Wii DSP toolchain, irrelevant to Wii U.

None of these plausibly explains a floating-point result mismatch, but rule them out before blaming
the emulator for anything exotic.
