# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this fork is

A hard fork of Cemu (Wii U emulator, C++20) retargeted at **Apple Silicon + macOS 26 only**. Upstream Cemu is portable; this fork is deliberately not. Three backends became one, two architectures became one:

- **arm64 only.** `precompiled.h` `#error`s on any other target. `BackendX64/` and all x86 IML machinery are deleted.
- **macOS 26.0 minimum.** Verify with `otool -l bin/... | grep -A4 LC_BUILD_VERSION` → `minos 26.0`.
- **Metal only.** OpenGL, Vulkan/MoltenVK, the GLSL shader emitter and glslang are deleted. The binary links Metal and QuartzCore, nothing else graphical.

Do not reintroduce portability shims, `#ifdef ARCH_X86_64`, or a second renderer. If something looks like it needs a runtime arch/backend check, it doesn't.

Planning docs live in `docs/porting/`. `00-master-plan.md` is the staged plan and risk register; the three numbered files are the detailed per-workstream designs (foundation/platform, CPU/JIT/memory, graphics/Metal). **Read the relevant one before touching that subsystem** — they contain verified line-level findings that are expensive to rediscover.

## Build and run

```sh
export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"        # first build is ~25 min without this
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build                                            # ~2 min clean on an M2
./bin/Cemu_relwithdebinfo --verbose -g "Roms/<title>.wux"
```

`MACOS_BUNDLE=OFF` for day-to-day work — bundle builds are for signing/entitlements. `RelWithDebInfo` deliberately has **LTO off**: it keeps relinks fast and keeps `.o` files as real Mach-O, so `llvm-objdump` can inspect generated code. `Release` turns ThinLTO on and the objects become LLVM bitcode.

Useful launch flags (`src/config/LaunchSettings.cpp` has the full list): `--force-interpreter` to bypass the recompiler when isolating a JIT bug, `--ppcrec-lower-addr` / `--ppcrec-upper-addr` to bisect which recompiled function broke.

There is **no test suite.** Verification is: it builds, it boots a title, the frame looks right, and nothing new appears in the log.

## Verifying a change

A green build proves very little here. The real gate is booting a real title:

```sh
./testing/capture-scene.sh <pid> <scene-name>    # window-only frame + FPS/RSS/threads -> testing/golden/baseline.tsv
```

`testing/golden/baseline.tsv` is the committed record of every measurement; the PNGs and traces are gitignored (large, machine-specific). Requires `~/Library/Application Support/Cemu/keys.txt` with the Wii U common key and the title's disc key — without it decryption fails and nothing boots. Game images live in `Roms/` (gitignored).

Cemu writes no `log.txt` until clean exit, and `CemuApp::OnExit` calls `_Exit()`, so **`kill -9` loses buffered log and shader-cache writes.** Use `--verbose` and read stdout instead.

When a launch appears to hang at low CPU, it is almost always a modal dialog. Read it without screenshotting the user's screen:

```sh
osascript -e 'tell application "System Events" to tell (first process whose unix id is <pid>) to tell (first window whose description is "alert") to get value of every static text'
```

## Profiling — use xcprof, not raw xctrace

`xcprof` (Axiom, on PATH) wraps xctrace and emits structured output. It is the fastest way to get attributable numbers, and `compare` is what makes optimization claims defensible:

```sh
xcprof record --attach <pid> --preset cpu --time-limit 15s --no-prompt --output testing/traces/<name>.trace
xcprof analyze testing/traces/<name>.trace
xcprof compare testing/traces/before.trace testing/traces/after.trace
```

`--output` must stay inside the repo (its sandbox) or pass `--allow-external-output`. `xcsym crash <file>` symbolicates `.ips`/MetricKit crash reports.

A measured baseline, MK8 title screen, 15s: `mach_continuous_time` **47% self time**, with `now_cached()` → `steady_clock::now()` → `clock_gettime` accounting for ~55% inclusive, plus `PPCTimer_getFromRDTSC()` at 3.7% self and `shared_mutex` try/unlock around 10%. That is a mostly-idle scene dominated by `__OSCheckSystemEvents` polling, so it is **not** representative of in-game load — but it independently confirms the planned `PPCTimer` rewrite (`cntfrq_el0` instead of a 3-second calibration, and dropping the global spinlock + 128-bit divide) is aimed at the right place.

For Metal work: `MTL_HUD_ENABLED=1` for a frame-time overlay, `MTL_DEBUG_LAYER=1` + `MTL_SHADER_VALIDATION=1` for validation, and the in-app Debug menu has GPU capture wired to `MTL::CaptureManager`.

## Architecture worth knowing before editing

**Guest CPU.** PPC → IML (an SSA-ish IR) → AArch64, in `src/Cafe/HW/Espresso/Recompiler/`. `PPCRecompilerImlGen*.cpp` lowers PPC to IML; `IML/IMLOptimizer.cpp` runs the passes; `BackendAArch64/` emits code via the `xbyak_aarch64` submodule. There is currently **no backend peephole pass** — the only one that existed was x86-specific and was deleted. The `IMLUtil_*` helpers in `IMLOptimizer.cpp` were kept as substrate for an AArch64 replacement.

`BackendAArch64.cpp` has 13 `static_assert`s pinning `PPCInterpreter_t` field offsets into AArch64's scaled-imm12 addressing range. **Any field added to or removed from `PPCState.h` is checked by these at compile time** — that is by design, not an obstacle.

Guest threads are `ucontext` fibers (`src/util/Fiber/FiberUnix.cpp`), not host threads. `makecontext` passes `int` arguments, so the 64-bit fiber parameter is split across two — hence `__OSFiberThreadEntry(uint32 _high, uint32 _low)`.

**Guest memory** is pure fastmem: one 4 GB `PROT_NONE` reservation, sub-ranges `mprotect`'d on demand, guest→host is `memory_base + addr` with no bounds check. Unmapped accesses fault to SIGSEGV. **Apple Silicon uses 16 KB pages** — `MemMapperUnix.cpp` and the `MMURange` table in `MMU.cpp` still contain 4 KB assumptions (see `02-cpu-jit-memory.md`).

**Graphics.** `LatteThread` is the single GPU command-processor thread; all renderer calls happen there. Latte (R700) shaders are decompiled to **MSL source text** and compiled at runtime — there is no compiled-shader cache, so every shader recompiles on every launch. The Metal backend keeps exactly one encoder alive at a time, so any mid-frame texture/buffer upload tears down the render pass.

**Threading.** ~40-50 threads with **zero QoS annotations** on a 4P+4E machine. `g_CPUFeatures` now exposes `performanceCores`/`efficiencyCores` for sizing worker pools; `hardware_concurrency()` reports 8 and causes the shader compiler pools to oversubscribe. Note `FSpinlock` is a pure spin with no priority donation — adding QoS before replacing it with `os_unfair_lock` converts a latent bug into a reproducible hang.

## Style

`.clang-format` exists; don't reformat whole files. From `CODING_STYLE.md`: `m_` for members, `s_` for statics, camelCase variables / PascalCase functions and types, braces on their own line. **Use Cemu's fixed-width types** (`uint32`, `sint32`, `uint64`, …) rather than `uint32_t` or `int`.

## Editing traps hit repeatedly in this codebase

- **Deleting a backend exposes hidden coupling that only the linker finds.** The MSL emitter forward-declared four helpers that were *defined in the GLSL emitter's* translation unit; several files received headers (`robin_hood`, `StringBuf`) only transitively through Vulkan/Zir includes.
- **Scripted edits fail silently.** Source uses tabs; a heredoc written with spaces will no-op a Python `str.replace` and report success. Regex-based block removal is worse — a greedy pattern once deleted 474 lines including the NEON code it was meant to keep. Prefer explicit line ranges, and always `git diff --stat` before building.
- **Probe the pattern the code actually uses, not the one the docs describe.** Apple documents a one-`MAP_JIT`-region-per-process limit; it is not enforced on macOS 26 (4000 succeed under hardened runtime). A first probe that "confirmed" the limit was testing an RWX pattern xbyak never uses. Probes preserved in `tools/probes/`.

## MCP servers worth reaching for

- **sosumi** — Apple documentation. Use it to confirm macOS 26 API availability rather than relying on memory; it is how `MTLSamplerDescriptor.lodBias` was confirmed as new in 26.0 and how the MetalFX input requirements were checked.
- **context7** — third-party library docs (wxWidgets, SDL3, cubeb, vcpkg).
