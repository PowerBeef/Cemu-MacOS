# AGENTS.md

Project instructions for agents working in this repository (Grok-native).

## What this fork is

A hard fork of Cemu (Wii U emulator, C++20) retargeted at **Apple Silicon + macOS 26 only**. Upstream Cemu is portable; this fork is deliberately not. Three backends became one, two architectures became one:

- **arm64 only.** `precompiled.h` `#error`s on any other target. `BackendX64/` and all x86 IML machinery are deleted.
- **macOS 26.0 minimum.** Verify with `otool -l bin/... | grep -A4 LC_BUILD_VERSION` → `minos 26.0`.
- **Metal only.** OpenGL, Vulkan/MoltenVK, the GLSL shader emitter and glslang are deleted. The binary links **no graphics API but Metal** — `otool -L` shows Metal and QuartzCore and no GL/Vulkan/MoltenVK. (CoreGraphics/ImageIO/AppKit are still there for the wx UI; that is expected.)

Do not reintroduce portability shims, `#ifdef ARCH_X86_64`, or a second renderer. If something looks like it needs a runtime arch/backend check, it doesn't.

## Where the deep context lives

| Doc | Role |
|-----|------|
| `docs/porting/00-master-plan.md` | Staged plan, risk register, measured conclusions |
| `docs/porting/01-foundation-platform-packaging.md` | Platform / packaging |
| `docs/porting/02-cpu-jit-memory.md` | CPU / JIT / memory |
| `docs/porting/03-graphics-metal.md` | Graphics / Metal |
| `docs/testing/00-test-strategy.md` | Test strategy, provenance, licence |
| `docs/status/index.html` (+ `ledger.json`) | Live record of every attempt and measurement |

**Read the relevant porting doc before touching that subsystem** — line-level findings are expensive to rediscover. **Check the status page** for related landed/refuted items before proposing the same experiment again.

## Standing rules (always loaded under `.grok/rules/`)

- **Status ledger:** `.grok/rules/status-tracker.md` — when work lands, claim commits in `docs/status/ledger.json`, run `python3 docs/status/build-status.py`, commit the HTML. Negative results (`refuted` / `cancelled` / `reverted`) are first-class.
- **Measurement:** `.grok/rules/measurement.md` — read **before any A/B, telemetry, fps, or counter claim**.
- **Tools:** `.grok/rules/tooling.md` — MCP/CLI matrix (sosumi, context7, cmake/Ninja, xcprof, cemu-re, dkp).

**A claim about another project carries a date, or it does not get made.** Name the version, date or commit you checked.

**`--verify`** requires every measurement in `README.md` to appear verbatim in the ledger (`WARN`). Deliberately README only — not this file.

## Build and run

```sh
export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"        # first build is ~25 min without this
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build                                            # ~2 min clean on an M2
./bin/TesseraEmu_relwithdebinfo --verbose -g "Roms/<title>.wux"
```

`MACOS_BUNDLE=OFF` for day-to-day work. `RelWithDebInfo` has **LTO off** (fast relinks, real Mach-O objects). Useful flags: `--force-interpreter`, `--ppcrec-lower-addr` / `--ppcrec-upper-addr`, `--telemetry`, `--telemetry-areas`, `--forward-console-logging`, `--enable-gdbstub` (port 1337, for **cemu-re** MCP).

**Homebrew test ROMs** need official devkitPPC: `testing/toolchain/README.md` (`dkp-pacman -S wiiu-dev` → `/opt/devkitpro`). Suites: `testing/cpu-tests/` (ppc750cl), `testing/graphics-tests/` (self-dependency), `testing/rom-tests/`. See `docs/testing/00-test-strategy.md`.

## Verifying a change

A green build proves very little. The real gate is booting a real title (keys in `~/Library/Application Support/TesseraEmu/keys.txt`, images in `Roms/`):

```sh
./testing/capture-scene.sh <pid> <scene-name>    # -> testing/golden/baseline.tsv
```

`log.txt` flushes continuously; shader-cache writes may be lost on hard kill. Guest exit path can crash (`b4d3d82`) — do not rely on clean shutdown. Modal hang: read the alert via Accessibility on the pid (see measurement rule for scripting notes).

**Profiling:** use **`xcprof`**, not raw xctrace — details in `.grok/rules/measurement.md` and tooling rule.

## Architecture worth knowing before editing

**Guest CPU.** PPC → IML → AArch64 in `src/Cafe/HW/Espresso/Recompiler/`. No AArch64 backend peephole yet. `BackendAArch64.cpp:367-379` pins `PPCInterpreter_t` field offsets with `static_assert`s — changing `PPCState.h` is checked at compile time. Guest threads are `ucontext` fibers (`FiberUnix.cpp`); `makecontext` splits the 64-bit arg → `__OSFiberThreadEntry(uint32, uint32)`.

**Guest memory.** Pure fastmem: 4 GB `PROT_NONE` reservation, `mprotect` on demand, `memory_base + addr`. Unmapped → SIGSEGV. **16 KB pages** — `MemMapperUnix` and `MMURange` entries are page-aligned (Stage 3); see `02-cpu-jit-memory.md` §4.3.

**Graphics.** `LatteThread` owns command encoding/state; shader/pipeline compile pools call into Metal from other threads. Latte shaders → MSL text at runtime. Pipeline cache stores **descriptors**, not GPU binaries — MSL still recompiles each launch. One encoder at a time: mid-frame **texture** upload tears down the pass; **buffer** uploads use `DeviceShared` memcpy. See `03-graphics-metal.md`.

**Threading.** QoS via `SetThreadName(name, ThreadRole)`. `FSpinlock` is `os_unfair_lock`. Size pools with `g_CPUFeatures` P/E counts, not raw `hardware_concurrency()`.

## Style

`.clang-format` exists; don't reformat whole files. `CODING_STYLE.md`: `m_` members, `s_` statics, camelCase vars / PascalCase functions and types, braces on their own line. **Cemu fixed-width types** (`uint32`, `sint32`, `uint64`, …) not `uint32_t`.

## Editing traps

- Deleting a backend exposes hidden coupling (helpers/headers that lived only in the removed unit).
- Scripted edits fail silently — sources use **tabs**; prefer line ranges; `git diff --stat` before build.
- Probe the pattern the code uses, not the docs (`tools/probes/`).
- **`WaitForCompiled()` can return true on a failed Metal compile** — check the function pointer, not the bool (`92a9885`).
- A `255` sentinel in a small index array is a read/write footgun (and `6+255` wraps in `uint8`).
