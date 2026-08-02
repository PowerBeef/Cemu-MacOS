# TesseraEmu — an Apple Silicon / macOS 26 native fork of Cemu

## Context

`/Users/patricedery/Coding_Projects/TesseraEmu` began as PowerBeef/Cemu-MacOS (renamed to PowerBeef/TesseraEmu once the fork had diverged enough to warrant its own identity) — a macOS fork of the Cemu Wii U emulator (~333k LOC C++20). It carries a working AArch64 recompiler and a ~7,300-LOC Metal renderer, but it is fundamentally still a *port*: it builds for x86_64 and arm64, ships three renderer backends, defaults to Vulkan-over-MoltenVK on macOS, targets macOS 13.4, and shows a startup dialog telling the user the macOS build is "purely experimental… degraded performance due to the use of MoltenVk and Rosetta for ARM Macs."

The goal is a hard fork that stops being a port: **arm64-only, macOS 26.0 minimum, Metal-only**, with the Apple Silicon–specific work that the portable codebase structurally cannot do.

Target/dev machine: **Apple M2 (4P+4E), 8 GB unified memory, 16 KB pages, macOS 26.5.2, Xcode 26.6 / Apple clang 21, cmake 4.3.2, ninja 1.13.2.** MoltenVK is not installed — which currently blocks a bundle build outright, and stops mattering once Vulkan is deleted.

Research pass (three parallel deep-read agents + my own verification) produced a full defect and opportunity map. Highlights that shape the plan, each verified directly against the source or the installed SDK:

- **A live heap-corruption bug in the Metal renderer, root-caused.** `MetalRenderer.cpp:239` loops `i < METAL_SHADER_TYPE_TOTAL` (=4) over `m_uniformBufferOffsets[METAL_GENERAL_SHADER_TYPE_TOTAL][MAX_MTL_BUFFERS]` (=`[3][31]`), writing 248 bytes past the array — ~128 of them past the end of `m_state` entirely. This is exactly the symptom the `// HACK: for some reason, this variable ends up being initialized to some garbage data` comment at `:265-268` describes. Everything else in the renderer is built on top of this.
- ~~**`MAP_JIT` allows exactly one region per process** under hardened runtime, making the JIT allocator a hard blocker.~~ **Corrected by measurement — see the R1 probe result below.** Apple's docs say this, but it is not enforced on macOS 26.5.2: 4000 regions succeed under `--options runtime` + `allow-jit`, and the pattern xbyak actually uses works signed. The recompiler still leaks every block it compiles (`PPCRecompiler_cleanupAArch64Code` is never called), so the arena remains worth building — but on merit, not as a gate.
- **`MemMapper::FreeMemory` never page-aligns and discards the return value.** With 16 KB pages, unmapping `mmuRange_HIGHMEM` (`0xFFFFF000`, which is `0x3000` into a page) returns `EINVAL` silently, so guest memory survives a title switch.
- **`MTLSamplerDescriptor.lodBias` is `API_AVAILABLE(macos(26.0))`** — confirmed in the installed SDK. The `// TODO: set lod bias` at `MetalSamplerCache.cpp:156` exists because the API didn't exist. Our deployment target is precisely the version that adds it.
- **No compiled-shader cache on Metal.** Every MSL shader recompiles from source on every launch (Vulkan caches SPIR-V; Metal caches nothing). The disabled replacement shelled out to `diskutil erasevolume` to build a RAM disk and ran `xcrun metal` per shader.
- **Zero QoS anywhere**, while up to 17 shader/pipeline compile threads compete with 3 guest cores and the render thread for 4 P-cores.
- `dependencies/metal-cpp` is pinned at `a63bd172` (macOS 14.2 era) — no `MTL4*`, no `MTLResidencySet`, no MetalFX, no `CAMetalLayer` properties.
- `PPCTimer` spends **3 real seconds at every launch** calibrating a frequency that `cntfrq_el0` returns in one instruction, then reads the clock under a global spinlock with a 128-bit divide.
- Recompiled code is **never freed** — `PPCRecompiler_cleanupAArch64Code` is defined and never called.

All 7 submodules are currently uninitialized, so nothing arm64- or Metal-related configures yet.

---

## Approach

Four workstreams. **F** (foundation/platform/packaging), **J** (CPU/JIT/memory), **G** (graphics/Metal), plus **M** (measurement), which gates claims in J and G. Every numbered step below ends at a buildable, launchable checkpoint.

Detailed per-workstream designs live alongside this file:

- `01-foundation-platform-packaging.md` — build system, platform layer, W^X/`MAP_JIT`, QoS, signing/notarization, crash handling
- `02-cpu-jit-memory.md` — AArch64 recompiler codegen, IML, timers, fibers, 16 KB pages, AES
- `03-graphics-metal.md` — Metal renderer correctness, shader binary cache, TBDR exploitation, presentation, MetalFX

### Stage 1 — Bootstrap and de-risk (do first, in this order)

1. **Init 6 submodules** (skip `Vulkan-Headers`): `git submodule update --init --depth 1 dependencies/{vcpkg,ZArchive,cubeb,imgui,metal-cpp,xbyak_aarch64}`. Let vcpkg clone fully.
2. **Bump `dependencies/metal-cpp` to `2948dd1e`** (metal-cpp_26.4). Gates the LOD-bias fix, `CAMetalLayer` properties, MetalFX, and any Metal 4 evaluation.
3. **G0.1 — Fix the ctor overflow.** `MetalRenderer.cpp:239` → `METAL_GENERAL_SHADER_TYPE_TOTAL`; delete the HACK block at `:265-268`. Also: retain `m_device` on the `CopyAllDevices` path (`:264-277` — asymmetric ownership vs. the unconditional release in `~MetalRenderer`), null-guard `GetAndRetainCurrentCommandBufferIfNotCompleted` (`MetalRenderer.h:288-295`), and initialize `m_recordedDrawcalls{0}` / `m_commitTreshold{0}` (`:538-540`).
4. **G1.1 — `kDefaultGraphicsAPI = kMetal`.** `CemuConfig.h:76-82` and `ActiveSettings.cpp:109-133`. Ship this alone: today an out-of-the-box macOS build runs MoltenVK, so **nobody has been exercising the Metal path by default**. Establish that it boots before deleting the fallbacks.
5. **G1.2–1.3 — Delete OpenGL and Vulkan.** `Renderer/{OpenGL,Vulkan}/`, the GL/VK canvases, `imgui_impl_{opengl3,vulkan}`, `Vulkan-Headers`, `glslang` (from `vcpkg.json` *and* `overrides`). This is what unblocks `MACOS_BUNDLE=ON` on this machine — the MoltenVK `FATAL_ERROR` probe at `src/CMakeLists.txt:127-135` dies with it. **Read and save `CachedFBOVk.cpp:198-240` + `VulkanRendererCore.cpp:1187-1222` before deleting** — that's the reference design for G3.1.
6. **R1 probe — the single most schedule-relevant experiment.** Before investing anywhere else: build `MACOS_BUNDLE=ON`, ad-hoc-sign with `com.apple.security.cs.allow-jit` + `--options runtime`, boot a title. If it dies after the first recompiled function, the `MAP_JIT` one-region limit is confirmed and **J-Stage-3 becomes a blocker that must be pulled forward**.
7. **G0.5 / M — Stand up the harnesses.** Golden-scene screenshot set (8–10 scenes: BotW field+shrine, MK8, Splatoon ink, Smash training, Wind Waker HD sea, Xenoblade X, 3D World, a menu-heavy title) via the existing `HandleScreenshotRequest`. `MTL_DEBUG_LAYER=1` + `MTL_SHADER_VALIDATION=1`. UBSan unconditionally in Debug; ASan scoped to `src/Cafe/HW/Latte/` only (it interacts badly with `MAP_JIT`).

### Stage 2 — The purge (arm64-only, macOS 26)

8. **Delete `BackendX64/`** (9 files, ~8,500 lines incl. `x86Emitter.h`) — it currently compiles on arm64 as dead weight. Then the x86 branches: `PPCRecompiler.cpp:236-250/294-330/614-690`, `IMLOptimizerX86_SubstituteCJumpForEflagsJump`, the x86 half of `GetInstructionFixedRegisters`, `PPCREC_IML_OP_X86_CMP`, `namespace IMLArchX86`, the 15 `_x64XMM_*` fields in `PPCRecompiler.h:134-150`, and `PPCState.h:70`'s `temporaryGPR[4]`. **Keep** the `IMLUtil_*` helpers in `IMLOptimizer.cpp:449-529` — they're the substrate for the AArch64 peephole pass.
   *Careful:* `PPCRecompiler.cpp:678` allocates from `&_x64XMM_xorNegateMaskBottom` using `offsetof` arithmetic — delete the call, don't retarget it.
9. **Deployment target and platform purge.** `CMAKE_OSX_DEPLOYMENT_TARGET "26.0"`, force `CMAKE_OSX_ARCHITECTURES "arm64"`, `MACOS_BUNDLE` default **ON** (a non-bundle build can't be signed, so it can't use `MAP_JIT`). Delete Wayland/X11/GTK3/bluez/GameMode/XInput/DirectInput/DirectSound/XAudio, `dist/{linux,windows}/`, the Windows/Linux CI jobs, and the stale "experimental / Rosetta" disclaimer at `CemuApp.cpp:388-402`.
10. **Rewrite `cpu_features` for arm64** — `hw.optional.arm.FEAT_*` sysctls plus P/E core counts. Use them for the log line and AES dispatch only; **do not gate compiled code paths on them**, because clang 21 already targets `apple-m1` with `+lse +aes +sha2 +dotprod +fullfp16` unconditionally.
11. **Normalize `__arm64__` → `ARCH_AARCH64`** (`precompiled.h`, `FiberUnix.cpp:18`, `coreinit_Thread.cpp:18/55/1341`). `__arm64__` is an Apple-only spelling used where `__aarch64__` was meant.
12. **CMake hygiene.** Fix `if(CMAKE_C_COMPILER_ID STREQUAL "Clang")` at `src/CMakeLists.txt:37` — it never matches AppleClang *and* tests the C compiler for a C++ warning. Guard IPO with `check_ipo_supported()`; keep **ThinLTO for Release only** (full LTO will thrash 8 GB), add `-Wl,-cache_path_lto`, turn LTO **off** for RelWithDebInfo. Add `-fvisibility=hidden` + `-Wl,-dead_strip`.
    **Explicitly not doing:** `-mcpu=apple-m1` (verified no-op), `-fno-semantic-interposition` (ELF-only concept), BOLT (no Mach-O backend), PGO in the default pipeline (the hot code is JIT-generated and invisible to it).

### Stage 3 — Correctness foundations

> **Status: complete.** 16 KB pages fixed and verified with a probe; the JIT
> size/cursor bug fixed (it was truncating the I-cache flush range, not just the
> size metric); signing wired into CMake and a signed hardened-runtime bundle
> verified running the JIT; crash diagnostics rewritten for arm64.
> **§14 (JitCodeArena) was measured rather than built** — see the note under
> §16 below.

13. **16 KB pages.** Add an `AlignRange()` helper to `MemMapperUnix.cpp` applied in **both** `AllocateMemory(fromReservation)` and `FreeMemory(fromReservation)` — round base down *and* end up — and check return values. Redeclare `mmuRange_HIGHMEM` as `{0xFFFFC000, 0x4000}` and `CORE0/1/2_LC` as size `0x8000`, making explicit what the kernel's rounding already does. Replace the 4 KB assert at `MMU.h:77` with round-up-and-log (graphic packs in the wild use 4 KB granularity — don't turn a working pack into an assert). Add a boot-time audit that logs every rounding-induced expansion and asserts no two rounded extents overlap.
14. **First-party `JitCodeArena`** — one 256 MB `MAP_JIT` region (256 MB keeps every intra-arena branch inside the ±128 MB range the jump ladder already enforces), bump+free-list suballocation, a reentrant per-thread `JitWriteScope` RAII guard around `pthread_jit_write_protect_np`, and `sys_icache_invalidate` inside a single `JitCodeArena::Publish()` that is **the only** writer of `ppcRecompilerDirectJumpTable`. Keep `xbyak_aarch64` as an encoder only; replace `AArch64Allocator`'s body and delete `setFreeDisabled`.
    **Do not adopt `com.apple.security.cs.jit-write-allowlist`** — it disables `pthread_jit_write_protect_np` entirely and would invert the emitter's control flow for zero benefit here.
15. **Sign from CMake, always.** Two entitlement files: release = `allow-jit` only; debug adds `get-task-allow`. Sign **inner-out** (nested dylibs, then the bundle), never `--deep`, never `--entitlements` together with `--preserve-metadata=entitlements`. Ad-hoc + hardened runtime + `allow-jit` is sufficient for `MAP_JIT` — **no paid Developer ID needed for local development**; document that in BUILD.md.
    Drop `allow-unsigned-executable-memory` (grants nothing on arm64 — plain RWX `mmap` fails regardless) and `disable-library-validation` (its only justification was MoltenVK, now deleted).
16. **JIT lifetime.** *(Partially done — the size/cursor bug is fixed; reclamation is deliberately not built. `PPCRecompiler_invalidateRange` has only three callers — GDB breakpoints, the debugger, and graphic-pack code patches — none of which occur in ordinary play, and a measured MK8 session leaked zero bytes. The leak is bounded by graphic-pack patching, not by playtime, so deferred reclamation is gated on the counter showing real growth rather than built speculatively against a use-after-free risk.)* Fix `x86Size = getMaxSize()` → `getSize()` and restore the emitter cursor after `processAllJumps()` (`BackendAArch64.cpp:1601-1616`) — today the recompiler dump writes trailing garbage and the size metric is meaningless. Then epoch-based deferred reclamation: retire list + per-core epochs, freed from the existing recompiler-thread wakeup, poisoned with `brk #0xdead` for a grace period in debug.
17. **Crash diagnostics.** `sigaltstack` + `SA_ONSTACK` per thread (today a stack-overflow SIGSEGV is unreportable), arm64 PC recovery via `arm_thread_state64_get_pc` (**not** a raw `__pc` cast — PAC), demangled macOS backtraces into the crash log plus raw UUID+slide for offline `atos`, and `DEBUG_BREAK` → `__builtin_debugtrap()`.

### Stage 4 — Scheduling and native integration

18. ~~**Fix core counts before adding any QoS.**~~ **Done** — `g_CPUFeatures.performanceCores`/`efficiencyCores`, and the pipeline-cache pool is now `clamp(efficiencyCores, 1, 8)`. Original: `GetPhysicalCoreCount()` returns 8 on M2, so `MetalPipelineCache.cpp:54` spawns 7 compile threads. Add `GetPerformanceCoreCount()` / `GetEfficiencyCoreCount()` from `hw.perflevel{0,1}.physicalcpu` and size **all** compile pools off the E-core count.
19. ~~**Replace `FSpinlock` with `os_unfair_lock` — before assigning QoS.**~~ **Done.** `fspinlock.h` is now `os_unfair_lock_lock/trylock/unlock`, so the kernel knows the owner and donates priority. The ordering hazard this item warned about (a `USER_INTERACTIVE` guest core spinning on `recompilerSpinlock` held by a descheduled `UTILITY` recompiler, a **hang** rather than a slowdown) was real, and was discharged by doing this before item 20 as instructed. Note `os_unfair_lock` exposes no "is it locked" query by design; the call sites that wanted one now use the ownership assertions.
20. ~~**QoS assignment**~~ **Done**, as `SetThreadName(name, ThreadRole)` rather than a separate `ConfigureThread` seam — the role is a second argument to the existing call, which every thread entry point already makes. `QosForRole` (`util/helpers/helpers.cpp:121`) maps role → class; 16 sites pass a role, and `Compiler`/`Background` deliberately use `UTILITY` rather than `BACKGROUND`, which macOS throttles hard enough to starve shader compilation during area loads. Original plan text: `USER_INTERACTIVE` for the 3 guest cores + `LatteThread` (exactly 4, matching the P-cluster), `UTILITY` for the recompiler and all shader/pipeline compile pools, `USER_INITIATED` for input, main thread untouched. Replace `ThreadPool::FireAndForget`'s detach-and-leak with `dispatch_async` — libdispatch already has a kernel-tuned P/E-aware pool.
21. **Audio.** Replace the `std::vector` + mutex in the cubeb realtime callback (`CubebAPI.cpp:40` — a mutex on the CoreAudio HAL thread, which runs above every QoS class, plus an O(n) `erase` from the front) with a lock-free SPSC ring. Clamp latency to `[480, 1920]` frames with a Low/Balanced/Safe setting, default Balanced.
22. **Native integration.** `NSHighResolutionCapable` (**without it macOS runs the app 1× magnified and the Metal drawable is half resolution**), `NSBluetoothAlwaysUsageDescription` (missing → the app is *killed*, not denied, on Wiimote access), `NSLocalNetworkUsageDescription` for DSU. Screensaver inhibition via `NSProcessInfo -beginActivityWithOptions:` in a new `ScreenSaverMac.mm` (not `IOPMAssertion` — an orphaned assertion keeps the display awake until reboot), which also removes the `SDL_INIT_VIDEO` call that is almost certainly the "feature crashes on macOS" cause. Then delete the macOS-only 5 ms `wxTimer` SDL pump and use the same dedicated SDL thread as every other platform. Replace `<Carbon/Carbon.h>` with four locally-defined key constants. Delete the self-updater (App Translocation makes `cp -rf` over a running bundle unworkable) — replace with "open releases page".
    ~~**Staying on SDL3; not adopting `GameController.framework`**~~ — **reversed and done.** The deferral reasoning (a sixth provider plus two mapping databases) was accepted at the time; a `GameController` provider was subsequently added anyway (`src/input/api/GameController/`) and SDL3 retained alongside it for the Wiimote HID transport.
    **Game Mode needs no code** — it triggers on `LSApplicationCategoryType = public.app-category.games` (already set) + real AppKit fullscreen. Verify empirically via `log stream --predicate 'subsystem CONTAINS "gamepolicy"'`.

### Stage 5 — Performance (measured, incremental)

**M — build the measurement harness first; nothing below is claimable without it.** A `--jit-audit` mode that force-compiles every function at load and emits per-function CSV (`ppcInstrCount, imlCount, spill/fill, hostBytes, hostInstrCount`), with **`hostInstrCount / ppcInstrCount`** as the headline number — deterministic and diffable across commits. Plus PPC and IML opcode histograms, so `rev16` gets sized *before* a day is spent on it. Plus a JIT symbol map side-file for `xctrace`, and an interpreter↔JIT differential fuzzer (randomized operands weighted toward denormals/±0/±inf/sNaN) that **gates the FMA work**.

**J — CPU, ordered by payoff/effort:**
- **Offset-0 addressing fold** (S) — `load`/`store`/`fpr_load`/`fpr_store` unconditionally emit `add_imm` even when `memOffset == 0`. Biggest instruction-count win in the plan.
- **FP load/store via `rev32`/`rev64`** (S–M) — removes both an instruction and a ~5-cycle GPR↔FPR domain crossing from every float memory op.
- **`rev16`** (S) — 3 instructions → 2 on `lhz`/`sth`/`lhbrx`/`sthbrx`.
- **AArch64 bitmask immediates** (S–M) for AND/OR/XOR. There are exactly **5,334** encodable 32-bit values — enumerate all of them in the predicate's test.
- **`ldp`/`stp` fusion** for name spill/fill (M) — in the backend emit loop, *not* as an IML pass, so DCE/RA/debug-printer are untouched.
- **Cycle counting as an IML register** (M) — biggest loop win: 3 instructions per basic block plus a load per back-edge collapse to one `sub`. Highest-risk item; needs a debug-build shadow counter.
- **FMA** (M) — this is a **correctness fix**: PPC `fmadd` is single-rounding and the current mul+add lowering double-rounds. No new IML type needed (`FPR_R_R_R_R` already exists with correct register-usage semantics). **The trap: PPC `fmsub` ↔ ARM `fnmsub` are swapped** — unit-test all four explicitly.
- **`PPCTimer` rewrite** (S–M) — `cntfrq_el0` instead of the 3-second calibration; the ratios are exact rationals (`3315/64`), so the global spinlock, `dmb ish`, and 128-bit divide all disappear from a function called on every guest `mftb`.
- **Fiber switch in hand-written AArch64 asm** (M) — `swapcontext` calls `sigprocmask` on *every* guest thread switch. Save x19–x30, d8–d15 low halves, sp, fpcr; `mmap`'d stacks with **16 KB** guard pages (today: `malloc`, no guard). **Must ship with CFI** — without it, `lldb bt` and Instruments' sampler produce junk, silently invalidating the whole measurement harness.
- **AES via FEAT_AES** (S–M) — **best payoff/effort in the plan and dependency-free; start it on day one in parallel.** Title decryption currently runs table-driven software AES; ~4–5× on WUA/WUD mount. Gotcha: ARM `AESE` does AddRoundKey *first*, so round-key indexing is offset by one vs. the x86 source — write it against FIPS-197 vectors, not against the x86 code.
- **`psq_l`/`psq_st` via `fcvtl`/`fcvtn`** (M) — ~10 instructions → 4 on the hottest FP memory op in Wii U graphics code.

**G — graphics, ordered by payoff/risk:**
- **LOD bias** (S) — 3 lines, now that macOS 26 provides the API. Fixes mip selection in every game using LOD bias.
- **`MTLBinaryArchive` shader cache** (M) — **largest user-visible win.** Archives carry *both* GPU binaries and the AIR slice, so an OS update degrades to "skip the frontend" rather than "recompile everything". Owned by `MetalPipelineCache` (one archive per title), keyed on `{titleId}_{gpuArch}_{osBuild}_{LatteShaderCache_getPipelineCacheExtraVersion}` so MSL-emitter edits self-invalidate. Delete the RAM-disk/`xcrun` code and the `system()` helpers in `MetalCommon.h`.
- **Presentation fixes** (S each) — wire `displaySyncEnabled` so the vsync setting stops being a no-op; set `maximumDrawableCount`; use `presentDrawable:afterMinimumDuration:` for frame pacing; stop mutating the live `CAMetalLayer`'s `pixelFormat` per frame (do sRGB in the output shader instead — `MetalOutputShaderCache` already keys on it).
- ~~**Re-test `Host` buffer-cache mode** (S) — the true zero-copy path.~~ **Done — `Host` tested and rejected; `DeviceShared` adopted instead.** `Host` is measurably no better than `DeviceShared` (150.2–152.7 passes/f vs 149.0–150.1) despite also removing the memcpy. Switching Auto from `DevicePrivate` to `DeviceShared` was the win: −17% render passes, −16% GPU time. Details at the end of this document.
- **`D24_S8` decoder** (M) — `depth24Stencil8PixelFormatSupported` is **false on all Apple Silicon**, so the remap-to-`Depth32Float_Stencil8`-with-the-decoder-commented-out branch at `LatteToMtl.cpp:173-179` is a live corruption path on every M-series Mac. Add `logOnce` on unknown formats *first* to get the real priority order.
- **Render-pass self-dependency** (L) — largest correctness win. Port the *current* Vulkan design (`CheckForSelfDependency`: monotonic stamp + intersect against bound textures, O(bound), once per FBO/binding change), **not** the commented-out per-draw shader-scanning version. Apple-specific win: where the dependency is pixel-only, framebuffer fetch already handles it at zero cost.
- **Memoryless depth + load/store audit** (M) — largest TBDR win. `CachedFBOMtl` uses `LoadActionLoad`/`StoreActionStore` on *every* attachment of *every* pass; on a TBDR GPU loading an attachment you're about to fully overwrite is pure wasted bandwidth. Depth that's never sampled becomes `Memoryless` + `Clear`/`DontCare`: ~16 MB and its full write bandwidth per 1080p target, 64 MB at 4×.
- **Encoder/commit fixes** (S each) — re-enable `addCompletedHandler` (handler only enqueues; the Latte thread drains), revive the empty `NotifyLatteCommandProcessorIdle`, use `RequestSoonCommit` on readback.
- **Drop dead branches** (S) — mesh-shader/Metal3/Apple-GPU/vendor-sniffing checks are all compile-time true on Apple7+; the silent drop of GS/RECTS draws becomes an assert.
- **MetalFX spatial** (M) — replaces the hand-written bicubic output shader. **Temporal and frame interpolation are rejected**: they require per-pixel motion vectors and a camera matrix, which a Wii U emulator structurally cannot produce.
- **Compile-thread unification** (S) — three pools with three uncoordinated sizing policies, up to 17 threads. Size from E-core count, `QOS_CLASS_UTILITY` (not `BACKGROUND` — it gets throttled), and check `PreponeCompilation`'s wait primitive for priority donation.

**Metal 4: evaluate, don't migrate.** One timeboxed one-day spike after the binary archive ships — determine whether an `MTLRenderPipelineState` from `MTL4Compiler` works with a *classic* `MTLRenderCommandEncoder` (Apple documents neither compatibility nor incompatibility). If yes, adopt `MTL4Compiler` as a compilation backend only. Full command-encoding migration is not worth it: argument tables and mandatory residency sets are pure cost at Cemu's binding counts, and Metal 4 barriers don't solve intra-pass self-dependency either.

### Explicitly not doing

Argument buffers · `MTLResidencySet` · `MTLHeap` aliasing · EDR/HDR · MetalFX temporal/frame-interpolation · reviving the RAM-disk AIR cache · offline shader precompilation · reviving `IMLReg::Offset` for sub-register views (partial-def semantics on a linear-scan allocator with 7 open TODOs — and NEON has no non-destructive 3-operand vector FMA, so the ceiling is near zero) · vector `fmla` for `ps_madd` · guest FPSCR rounding-mode plumbing · shrinking the 512 MB jump table (it's reserved VA; the flat table is one `ldr` on the hottest path) · `-mcpu`/BOLT/PGO/full LTO · Mach exception ports · static libusb (LGPL) · hand-rolled updater or thread pool · `jit-write-allowlist`.

---

## Critical files

| File | Role |
|---|---|
| `CMakeLists.txt`, `src/CMakeLists.txt` | deployment target, arch forcing, renderer purge, MoltenVK removal, codesign integration, LTO/IPO |
| `src/util/MemMapper/MemMapperUnix.cpp` | the 16 KB page fix (round both ends, check returns) |
| `src/Cafe/HW/Espresso/Recompiler/BackendAArch64/BackendAArch64.cpp` | `JitCodeArena` seam, all codegen wins, size/cursor bug |
| `src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp` | x86 purge, instance-data shrink, epoch reclamation, thread QoS |
| `src/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGenFPU.cpp` + `IML/IMLInstruction.h` | FMA, paired-single work |
| `src/Cafe/HW/Espresso/PPCTimer.cpp` | `cntfrq_el0` rewrite |
| `src/util/Fiber/FiberUnix.cpp` (+ new `.S`) | hand-written context switch, guard pages |
| `src/util/helpers/helpers.cpp` | `ConfigureThread` seam (QoS + name + sigaltstack), core counts |
| `src/Cafe/HW/Latte/Renderer/Metal/MetalRenderer.{cpp,h}` | ctor overflow, encoder/commit architecture, presentation |
| `src/Cafe/HW/Latte/Renderer/Metal/MetalPipelineCache.cpp` + `MetalPipelineCompiler.cpp` | `MTLBinaryArchive` home, compile-thread sizing |
| `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerAnalyzer.cpp` | the three interleaved `resourceMappingGL/VK/MTL` tables — riskiest edit in the GL/VK deletion |
| `src/Cafe/HW/Latte/Renderer/Vulkan/CachedFBOVk.cpp` | **read before deleting** — reference design for the Metal barrier mechanism |

---

## Verification

**Per-stage gates** (each must pass before the next stage):

1. **Bootstrap** — `./bin/TesseraEmu_relwithdebinfo` opens, game list populates, a title boots and renders on Metal. Golden-scene screenshots captured as the baseline.
2. **R1 probe** — bundle + ad-hoc sign + hardened runtime + `allow-jit`; boot a title. Records whether `MAP_JIT` reordering is needed.
3. **Purge** — `grep -rn "ARCH_X86_64\|__x86_64__\|__arm64__\|ENABLE_VULKAN"` returns only vendored third-party hits. `nm -u` shows no `vk`/`gl` symbols. Golden scenes render identically.
4. **16 KB pages** — debug self-test round-trips every `MMURange` twice asserting `mprotect` success; boot title A → game list → title B with no stale-memory assertion; `lldb` shows `---` at `memory_base + 0xFFFFF000` after unmap.
5. **JIT arena** — `vmmap $(pgrep Cemu) | grep -i jit` shows exactly **one** region; an hour of forced `PPCRecompiler_invalidateRange` shows the JIT dirty size plateauing, not growing. Deliberately remove `sys_icache_invalidate` in a scratch build and confirm it crashes — proves the call is load-bearing.
6. **QoS** — `xctrace record --template 'System Trace'`, 60 s of gameplay: the 3 guest cores + `LatteThread` on P-cores, compile threads on E-cores, zero blocked time on the audio thread.
7. **Signing** — `codesign --verify --strict`, `codesign -dv --entitlements -`; on a *different* Mac, download the DMG through a browser (to get the quarantine bit), open, drag, launch with no Gatekeeper prompt; `spctl -a -vvv` → `accepted, source=Notarized Developer ID`.

**Continuous:**
- **`hostInstrCount / ppcInstrCount`** from `--jit-audit`, diffed per commit. Every J-stage change must move it or be reverted.
- **Golden-scene screenshot diffs** after every G change. For the resource-mapping collapse specifically, **byte-diff the generated MSL** for all golden scenes — that's the single most important gate in the graphics work.
- **Differential fuzzer** (10⁷ iterations/opcode in CI) gating every FP change; explicit unit tests for the `fmsub`↔`fnmsub` crossover.
- **Timer drift** — `OSGetSystemTime()` vs `CLOCK_MONOTONIC` over 60 s, fail above 100 ppm.
- **Memory** — `task_vm_info.phys_footprint` logged every 30 s. Budget on 8 GB is tight: ~1 GB MEM2 + up to 448 MB jump table + 164 MB Latte buffer cache + Metal residency before textures.
- **Frame pacing** — Metal Performance HUD (`MTL_HUD_ENABLED=1`) and p99 frame time during first traversal of a new BotW region, cold vs. warm archive.

**Batching rule:** every change to `LatteDecompilerEmitMSL.cpp` invalidates the binary archive for all users. Batch MSL-emitter changes and bump the cache version once per release, not once per fix.

---

## Risk register (top 6)

| Risk | Why likely | Detect early |
|---|---|---|
| ~~`MAP_JIT` one-region limit breaks the recompiler under hardened runtime~~ **RETIRED — probe run, does not fire.** See below. | — | — |
| Missing `sys_icache_invalidate` after taking JIT ownership | Zero occurrences in `src/` today; hidden inside xbyak | Make `Publish()` the only jump-table writer with the invalidate inside; debug counter asserting publish == invalidate |
| QoS hang via `FSpinlock` inversion | Pure spin, no donation, genuinely shared across QoS tiers | **Do `os_unfair_lock` before QoS.** `sample $(pgrep Cemu)` during any freeze — a stack pegged in `FSpinlock::lock` is diagnostic |
| Cycle-counting-in-a-register scheduling divergence | Manifests through the scheduler, not the CPU: nondeterministic hangs reproducing 1-in-100 boots | Debug-build shadow `remainingCycles` asserted at every `leaveRecompilerCode` and HLE entry |
| FMA changes FP results in the last ULP | Games hashing floats or accumulating physics diverge → replay/ghost desync | Differential fuzzer; ship behind a `GameProfile` toggle; bisect with the existing `--ppcrec-range` flag |
| Wrong MSL after the resource-mapping collapse | Three binding tables interleaved in one analyzer with three counters | Byte-diff generated MSL for every golden scene; any difference means something broke |

### R1 probe result — measured 2026-07-25 on macOS 26.5.2 / M2

**R1 does not fire. The JIT arena is an optimization, not a blocker.** Probes preserved in `tools/probes/`.

- Apple's documentation states an app with the hardened runtime and `com.apple.security.cs.allow-jit` "can only create one memory region with the `MAP_JIT` flag set". **Measured: 4000 regions succeed**, ad-hoc signed with `--options runtime` and that entitlement. The limit is not enforced.
- The pinned `xbyak_aarch64` maps `PROT_READ|PROT_WRITE` **plus** `MAP_JIT` (not RWX), writes, then `mprotect`s to `PROT_READ|PROT_EXEC`. **That exact sequence works signed and unsigned.** Because the region is never writable and executable at the same time, no `pthread_jit_write_protect_np` is needed, and `mprotect` appears to cover the required cache maintenance.
- Consequence: **§15 (sign from CMake) no longer gates §14 (JitCodeArena)**, and §14 does not gate shipping. Build the arena for the reason that remains real — recompiled code is never freed — but schedule it on merit.

**Correction (measured later):** an earlier note here claimed xbyak performs no I-cache maintenance. That was wrong, and the grep behind it only covered the public headers. `CodeGenerator::clearCache` in `dependencies/xbyak_aarch64/src/xbyak_aarch64_impl.h:4292` calls `sys_icache_invalidate` on `__APPLE__`, and `ready()`/`readyRE()` invoke it over `[getCode(), getCurr())`. The real defect was that `processAllJumps()` left the cursor short of the code end, so the flush range was truncated — fixed in the AArch64 backend rather than by taking ownership of the allocator.

Methodological caution, learned the hard way here: the *first* version of this probe allocated RWX and toggled `pthread_jit_write_protect_np`. That pattern SIGBUSes on write and would have "confirmed" R1 — but it is not what the code under test does. Reading `xbyak_aarch64_code_array.h` was what corrected it. **Probe the pattern the code actually uses, not the one the documentation describes.**

**Non-obvious ordering dependencies:** `os_unfair_lock` **before** QoS · screensaver→`NSProcessInfo` **before** moving SDL off the main thread · the `getSize()` fix **before** the measurement harness (otherwise the size metric is garbage) · save the Vulkan self-dependency design **before** deleting Vulkan.

---

### Stage 5 progress — measured 2026-07-26 on macOS 26.5.2 / M2

**The plan's J/G ordering was wrong about where the CPU actually went, and profiling caught it before any codegen work started.** Both wins so far were idle-wait bugs, not the instruction-selection items the plan ranked first. Neither would have been found by `hostInstrCount / ppcInstrCount`.

Combined result on MK8 at a locked 60 FPS: **1.77x less CPU** (median 183.5% → 103.9% of one core), FPS unchanged, scene renders identically.

**1. The scheduler idle loop was burning a P-core on clock reads** (commit `a7ed8ed`). `mach_continuous_time` was **67.6% of all CPU cycles**. The caller was not the graphics code — it was `__OSThreadCoreIdle`, which spun with no yield and no backoff calling `__OSCheckSystemEvents()` (and therefore reading the clock) as fast as the CPU allowed whenever no guest thread was runnable. Fixed by parking on the existing run-queue semaphore with a 250 µs bound (new `CounterSemaphore::waitUntilNonZeroWithTimeout`). A runnable thread already increments and notifies that semaphore, so real wakeups stay immediate; the timeout only bounds how late a periodic system event is serviced while idle, well inside AX's 1.7 ms floor.

**2. The Latte ring-buffer wait spun instead of parking** (commit `612d064`). Next largest after the above: `_mm_pause` 16.0% self + `swtch_pri` 9.4%. The spin *body* is cheap — 80 `yield`s measure 41.7 ns — the cost is the iteration rate. Replaced with `ldxr`/`wfe` on the ring's write index (`TCLGPUWaitForRBData`). Probed first: bare `wfe` blocks **1269 ns avg (max 1375)** because Apple silicon implements a WFE timeout, so it can never stall the GPU thread; a store from another thread wakes it in **42–208 ns**, so latency is unchanged. ~30x fewer idle passes, producer side untouched.

**3. `PPCTimer` rewrite landed as planned, and fixed an accuracy bug.** `cntfrq_el0` is exactly 24 MHz, so `CORE_CLOCK/cntfrq = 3315/64` exactly and the conversion is one multiply and one shift; the 3-second boot calibration, the global spinlock, and the 128-bit divide are all gone. Verified over 20M random deltas spanning ~8 days of uptime: `(delta*3315)>>3 == floor(delta*CORE_CLOCK*8/cntfrq)`, zero mismatches. At 1x speed old and new agree exactly — but at reduced speeds **the old code was wrong**, truncating `>>shift` on every call and drifting 0.067% slow over 2M calls at 0.125x. The new form is exact at every speed.

`HighResolutionTimer::now()` likewise reads `cntvct_el0` directly instead of `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` — same counter, **bit-identical output measured**, 18.40 ns → 0.44 ns.

#### Measurement methodology — two traps hit here

- **MK8's attract mode is not a fixed workload.** It cycles demo scenes with very different draw loads, so two traces captured at different times are not comparable. The first A/B suggested 4.4x from sample counts; a proper run showed 1.77x. **Interleave variants inside one process** (a temporary runtime toggle flipped every 20 s), discard windows that straddle a switch, and report the median of n≥5 each.
- **`xcprof compare` reports share-of-CPU, not absolute CPU.** When total CPU drops sharply it labels everything that survived a "regression" — it flagged 15. Absolute `cputime` over a fixed wall window is the number to trust.

#### Where the CPU goes now

Real draw work, and encoder construction is conspicuous: `LatteCP_itIndirectBufferDepr` 30.1% incl · `DrawPassContext::executeDraw` 19.2% · `MetalRenderer::draw_execute` 16.3% · **`renderCommandEncoderWithDescriptor` 6.4% + `AGXG14GFamilyRenderContext init` 5.7%**.

**4. Sampler LOD bias, and a graphic-pack register-corruption bug** (commit `3bb1049`). `MTLSamplerDescriptor.lodBias` is `API_AVAILABLE(macos(26.0))`, which is why it was a bare TODO. Instrumenting MK8: **29 of 81 distinct samplers carry a non-zero bias** (−1.0, −1.5, −2.0 LOD), all previously sampling a blurrier mip than the game asked for. The override beside it was worse — it wrote through `samplerWords` into `LatteGPUState.contextNew.SQ_TEX_SAMPLER`, i.e. the emulated register file, so a pack's anisotropy setting permanently replaced the game's for every later draw. Overrides now apply to a local copy, which also keeps the sampler cache key correct for free.

### Render-pass churn — measured, and the obvious fix does *not* work

**MK8 runs 29 render passes for 51 draws: 1.75 draws per pass.** On a TBDR GPU each pass is a full tile-memory load and store of the attachment set, so this is the structural problem behind the 12% of CPU in encoder construction. Instrumenting the cause of every pass start, per frame:

| cause | passes/frame |
|---|---|
| previous encoder was Render (genuine target switch) | 11 |
| **previous encoder was Blit (a blit tore down the render pass)** | **13** |
| previous encoder was Compute | 0 |
| forced recreate | 0 |

So ~45% of render passes exist only because a blit encoder ran between draws.

The plan proposed re-testing the `Host` buffer-cache mode here, on the theory that `UploadToBufferCache` only uses a blit encoder in `DevicePrivate` mode (`Shared`/`Host` are a plain `memcpy`), and that Host may have been judged unreliable *because of* the ctor heap corruption since fixed. Tested all three modes:

- **`Host` works.** "Buffer cache type: host", 60 FPS, no errors, no crash. It is stable and available — worth recording, since it was previously untried on a fixed build.
- Render passes drop 29 → 26 and blit-caused passes 13 → 10 per frame in both `DeviceShared` and `Host`.
- **But CPU is unchanged.** 30 s of `cputime` each: DevicePrivate **89.7%**, DeviceShared **94.9%**, Host **88.7%** of one core. That spread is noise; DeviceShared is nominally *worse*. **No default was changed** — there is no evidence to justify it.

Caveat bounding all of this: the measurable scene is the static title screen (51 draws/frame). Buffer-upload traffic there is small, so this does **not** rule out `Host` mating well with real gameplay — it only says the benefit is unproven on the one workload that can be measured without driving a controller.

### The rest of the G-list TBDR work is not worth doing — measured

Followed the churn to its source and measured every proposed mitigation before building any of it. **All of them come up empty on this workload.** Attributing every blit call and every render-pass start, per 60 frames:

| blit call site | calls | of which tore down a live render pass |
|---|---|---|
| **`texture_copyImageSubData`** | **600** | **600 (100%)** |
| `UploadToBufferCache` (DevicePrivate only) | ~270 | ~175 |
| `texture_loadSlice` · readback · `CopyBufferToBuffer` · screenshot | 0 | 0 |

**1. Batching the blits does not work.** The churn is essentially one call site — `texture_copyImageSubData`, exactly 10 per frame — and *every* call finds a live render encoder, meaning no two are ever adjacent. There is nothing to batch. These come from `LatteTexture_copySlice`/`copyData`, the texture-cache view-coherency mechanism shared with the old GL/Vulkan backends: legitimate guest-driven work, not a Metal defect. Removing them means changing Latte texture-cache policy, which is a different and much riskier project.

**2. Deferred clears have zero opportunity here.** Both clear paths (`texture_clearDepthSlice`, `ClearColorTextureInternal`) spin up a dedicated render pass that only clears and stores, 3 per frame. Folding those into the next pass's `LoadActionClear` is the textbook fix, so it was measured first: of 180 clears per 60 frames, **0 were followed by a render pass using that texture as an attachment.** The optimization would fire never.

**3. Memoryless depth / `DontCare` is structurally blocked, and the two items are coupled.** The plan listed pass-count and load/store actions as independent, but they are not. Because a frame's rendering to one target is split across many Metal passes, `LoadActionLoad`/`StoreActionStore` is exactly what keeps pass N+1 seeing what pass N wrote. Only the *last* store per depth texture per frame is elidable — 1–2 of 26. Memoryless requires all usage of an attachment to sit in a single pass, which this design cannot provide until pass count itself drops.

**4. And none of it matters, because the GPU is idle.** Measured actual GPU execution time from `GPUEndTime - GPUStartTime` on every command buffer: **2.83 ms/frame, 17% of the 16.67 ms budget.** The remaining 83% is headroom. Every item above is a *bandwidth* optimization aimed at a GPU with nothing to do. The 12% of CPU in `renderCommandEncoderWithDescriptor` + `AGXG14GFamilyRenderContext init` is a host-CPU cost, and the only lever on it is pass count — bounded by the texture-cache copies in (1).

**Recommendation: stop here.** The one remaining lever is deferring `texture_copyImageSubData` to a pass boundary when the copy provably does not alias anything the live pass touches (the same machinery as the render-pass self-dependency item). That removes at most 10 of 26 passes, so roughly 4–5% of CPU, in exchange for a correctness-sensitive change to the texture cache. Not a good trade while the emulator already holds a locked 60 FPS at ~103% of one core. Revisit with a GPU-heavy title, where the bandwidth items may actually bind.

---

### CORRECTION (2026-07-26): the section above sampled the wrong scene

**Everything above was measured on MK8's title card. That is not a representative workload, and the "GPU is idle, reject all of it" conclusion does not survive contact with in-game rendering.**

MK8's attract mode drives itself into full demo races with no input at all — the earlier claim that a GPU-heavy scene needed someone to play was simply wrong. Sampling across the whole attract cycle in one run:

| | GPU ms/frame | % of 16.67 ms | passes/f | draws/f | draws per pass |
|---|---|---|---|---|---|
| title card | 2.6 – 3.0 | 16 – 18% | 29 | 51 | 1.75 |
| demo race, typical | 7 – 11 | 45 – 66% | 97 – 137 | 700 – 1200 | 7 – 9 |
| **demo race, peak** | **14.6** | **87.7%** | **222** | **1466** | 6.6 |

What this overturns:

- **"The GPU has 83% headroom" is false in gameplay.** It peaks at **87.7% of frame budget**. Any additional load — higher internal resolution, a heavier title — pushes it over. The TBDR bandwidth items are *not* aimed at an idle GPU and should be reconsidered on their merits.
- **"1.75 draws per pass is pathological" was a title-card artifact.** In-game it is **7–9 draws per pass**, which is unremarkable. The renderer is not churning passes the way the title card suggested.
- **The blit picture is entirely different in-game.** `texture_loadSlice` and `CopyBufferToBuffer` measured *zero* on the title card; in a race they reach **15,812** and **2,943** calls per 60 frames respectively.

What still holds, re-verified in-game:

- **`texture_copyImageSubData` remains unbatchable** — ~960 calls per 60 frames of which ~900 (94%) tear down a live render encoder, exactly as on the title card. These are individually interleaved between draws.
- **`texture_loadSlice` is already effectively batched** — 15,812 calls but only 980 teardowns (6%), so consecutive uploads already share one blit encoder. No work needed there.
- Blit-caused teardowns are ~20–25% of all passes in-game (≈23 of 97, ≈46 of 222), similar in proportion to the title card.

**Revised recommendation:** the memoryless-depth and load/store-action audit is back on the table and should be evaluated against a demo-race trace, not the title card. The deferred-clear idea stays dead (clears are only ~6/frame in-game). Re-measure `draws/f > 200` windows specifically; see the sampling caveat below.

**Method note, and the reason this correction exists:** MK8's attract cycle spends most of its wall-clock on the title card, so an unfiltered average or a short trace lands there and reads as a light workload. Gate on `draws/f > 200` to isolate race frames. This is the same non-fixed-workload trap recorded earlier, hit a second time in the opposite direction — the first time it inflated a speedup, this time it hid the entire GPU load.

---

### BotW: a repeatable, GPU-heavy measurement scene

> **Superseded in part.** The "108–147% of budget / GPU-bound" reading below divided by the
> **60 FPS** budget for a title that targets **30 FPS**. Against BotW's real 33.3 ms budget the
> GPU sits near 50% duty cycle and is *not* the limiter in this scene. See
> "The render-pass churn was buffer uploads, not texture copies" at the end of this document
> for the corrected numbers. The scene itself remains the right A/B target.

Breath of the Wild (US v208, update installed) at the Shrine of Resurrection, Link standing still:

| metric | value |
|---|---|
| **GPU time** | **18.0 – 24.5 ms/frame** (~108–147% of 16.67 ms, but see the correction above) |
| FPS | **23.95** (BotW targets 30) |
| CPU | 184% of one core |
| render passes / frame | 173.7 |
| draws / frame | 4838.8 |
| draws per pass | 27.9 |
| `copyImageSubData` blits | 4906 per 60 f, **3329 tearing down a live pass (~55/frame)** |
| `CopyBufferToBuffer` blits | 1440 per 60 f, 720 teardowns (~12/frame) |

**This retires the "reject all of it" conclusion for good.** The GPU is not idle with 83% headroom — it is *over* frame budget and is the reason BotW misses its 30 FPS target. Every item rejected on the idle-GPU argument is back:

- **Memoryless depth + load/store audit is now the top graphics item.** 174 passes per frame, each doing `LoadActionLoad` + `StoreActionStore` on every attachment, against a GPU that is already over budget.
- **Reducing pass count is justified too.** ~67 of 174 passes per frame (**38%**) are torn down by a blit. (Which blit turned out to matter: the *buffer*-upload blits, not the texture copies this bullet originally pointed at. Acted on — see the end of this document.)
- Draws-per-pass is a healthy 27.9, so the passes are doing real work. The problem is their *number*, and the per-pass attachment traffic that comes with it.

**This is the measurement scene to use from now on.** It is *exactly* repeatable — `draws/f` holds at 4838.8 ± 0.3 and `passes/f` at 173.7 across every sample — which is far better than anything MK8 offers, where the attract cycle constantly changes what it renders. Standing still in a heavy scene is the ideal A/B target.

#### Reproducing it without a controller, and without playing

No save file and no human input needed. Cemu's `controllerProfiles/` ships empty, which is why input appears not to work at all:

1. Write `~/Library/Application Support/TesseraEmu/controllerProfiles/controller0.xml` by hand — `<type>Wii U GamePad</type>`, `<api>Keyboard</api>`, `<uuid>keyboard</uuid>`. Button values are **macOS virtual key codes**, because `wxKeyEvent::GetRawKeyCode()` is a pass-through on macOS (`fix_raw_keycode`'s fixups are all inside `#if BOOST_OS_WINDOWS`). Mapping ids are `VPADController::ButtonId` (1=A, 2=B, 9=Plus, 11–14=dpad, 17–20=left stick).
2. Those same virtual key codes are what `osascript` sends, so `tell application "System Events" to key code 6` presses the mapped A button. `key down "w"` / `key up "w"` also work, which is what makes walking possible.
3. Boot BotW, raise the window, send a few `key code 6` presses to get through the title and the awakening cutscene. Link is then in control inside the Shrine.

The whole intro is scriptable this way. Camera control (right stick, mappings 21–24) was left unmapped in the profile used here — add it if you need to navigate further, e.g. out onto the Great Plateau, which is heavier still.

`testing/drive-botw.sh` automates all of it and is now the standard way to get to the measurement scene. **Two traps cost real time and are worth knowing:**

- **A bare `key code` tap usually does nothing.** Cemu samples keystate once per emulated frame — 33 ms at 30 FPS — so a synthetic press/release often falls entirely between two samples. Every button must be held (~150 ms). This is why input appears to "work sometimes".
- Installing the DLC adds a *"Downloaded DLC"* entry to the title menu, so a blind sequence of A presses walks into it instead of starting the game. Once a save exists, the script selects **Continue** rather than replaying the intro, which is both faster and more reliable.

---

### Memoryless-depth / load-store audit — result: the safe wins are small, the big one needs pass-count work first

Audited against the BotW shrine scene. Measured structure per frame:

| | |
|---|---|
| render passes | 173.7 |
| distinct FBOs | 101.2 |
| **passes re-targeting the same FBO as the previous pass** | **35.9 (21%)** |
| passes with a depth attachment | 63.5 |
| **passes where the depth texture is also sampled by a shader** | **0** |
| depth attachment size | 1280×720 |
| distinct depth textures / frame | 13 |
| **max passes writing one depth texture / frame** | **28** |

**Estimated attachment traffic** (assuming RGBA8 colour and Depth32Float — exact formats not confirmed, so treat as an order-of-magnitude figure):

```
colour load+store   174 passes x 2 x 3.7 MB = 1281 MB/frame
depth  load+store    64 passes x 2 x 3.7 MB =  468 MB/frame
                                      total ~ 1749 MB/frame = ~49 GB/s at 28 fps
```

against roughly 100 GB/s of unified memory bandwidth on an M2. Attachment load/store plausibly accounts for about half the machine's bandwidth, which is consistent with the GPU sitting over frame budget.

**What the audit rules out:**

- **Memoryless depth is not available.** It requires all use of an attachment to sit inside one pass. Measured: all 13 depth textures are written by more than one pass, and one is written by **28 passes per frame**.
- **Blanket `StoreActionDontCare` on depth would corrupt.** For the same reason — pass N+1 loads what pass N stored.

**What it confirms is safe but small:**

- **Depth is never sampled (0 of 63.5 passes).** So the *final* store of each depth texture in a frame is dead: nothing ever reads it. That is 13 dead stores per frame ≈ 48 MB/frame ≈ 1.3 GB/s. Real, but it needs last-use lookahead that the current architecture (render pass descriptors cached per FBO, store action fixed at encoder creation) does not provide.

**What the audit actually points at:** the 35.9 same-FBO splits per frame. When consecutive passes target the same FBO, the store-then-reload between them is pure waste — the data leaves tile memory and comes straight back. That is ≈529 MB/frame ≈ **14.8 GB/s**, the single largest addressable item found, and it is ~10x the depth-store win.

~~Those splits are caused by blits (`texture_copyImageSubData`) interrupting a live render pass. So **the deferred-copy design is the keystone, not an alternative**.~~

> **Wrong — retired by measurement.** The splits were caused by **buffer** uploads, not texture
> copies. Zero mid-pass `texture_copyImageSubData` calls are followed by a pass on the same FBO,
> so deferring them would have removed no render passes at all. The proposed deferred-copy design
> (queue the copy when provably disjoint, flush at pass end / on dst bind / at frame end) is
> retired unimplemented. See "The render-pass churn was buffer uploads, not texture copies" at
> the end of this document for the attribution and the change that did work.

---

### The render-pass churn was buffer uploads, not texture copies — and the deferred copy is retired

The section above concluded that blits interrupting a live render pass caused the 35.9 same-FBO
splits per frame, and that deferring `texture_copyImageSubData` was therefore "the keystone".
**That was wrong, and the deferred-copy design is now retired as a measured negative.**

#### What was measured

Every `texture_copyImageSubData` issued while a render encoder was live was classified against
the live pass's FBO, and a backtrace was captured at every render-pass teardown so that each
same-FBO split could be attributed to whoever ended the previous pass. BotW shrine scene,
steady state:

| | per frame |
|---|---|
| copies that tore down a live render pass | ~41 |
| of those, dst is an attachment of the live FBO | **0** |
| of those, src is an attachment of the live FBO | ~35 |
| **copies followed by a render pass on the same FBO** | **0** |

The last row is the one that matters, and it is measured purely from FBO pointers, so it carries
no classification uncertainty. **Deferring texture copies would have removed zero render passes.**
They land at genuine FBO transitions, where the pass was ending anyway. On top of that they are
mostly not deferrable in the first place — in ~35 of 41 the copy *reads* an attachment of the
live pass, which forces the pass to end no matter what.

#### What actually caused the splits

Backtrace attribution put every one of the top eight split causes in `GetBlitCommandEncoder()`,
reached from **buffer** work — not one texture copy:

| cause | splits/frame |
|---|---|
| `MetalMemoryManager::UploadToBufferCache` ← `LatteBufferCache_Sync` ← `draw_execute` | ~25 (summed over several stacks) |
| `bufferCache_copyStreamoutToMainBuffer` ← `LatteStreamout_FinishDrawcall` | 4.6 |
| `LatteTextureReadbackInfoMtl::StartTransfer` | 0.9 |

`UploadToBufferCache` dominates, and the reason is the buffer-cache storage mode. In
`DevicePrivate` the upload cannot be a memcpy: it allocates staging memory, copies into it, and
encodes a blit — and asking for a blit encoder tears down the live render pass. In `DeviceShared`
it is a plain memcpy into shared storage and no encoder is involved at all.

`InitBufferCache` picked `DevicePrivate` for every title except Wind Waker HD. Since this fork
only ever runs on unified memory, device-private storage buys nothing and costs a pass teardown
per upload. **Auto now selects `DeviceShared`.**

#### A/B, same scene, same binary, env-var override

| | passes/f | same-FBO splits/f | GPU ms/f | draws/f | FPS | CPU |
|---|---|---|---|---|---|---|
| `DevicePrivate` (was) | 176.4 – 181.3 | 35.6 – 40.8 | 18.17 – 18.85 | 1190 | 28.63 | 204.9% |
| **`DeviceShared` (now)** | **149.0 – 150.1** | **12.3 – 13.5** | **15.37 – 15.85** | 1187 | 28.63 | 205.3% |
| `Host` (zero-copy MEM2) | 150.2 – 152.7 | 13.6 – 16.2 | 15.67 – 16.38 | 1190 | 28.63 | — |

Ten consecutive 60-frame windows per variant; the ranges are full min–max and do not overlap on
any of the three metrics that moved. **−17% render passes, −67% same-FBO splits, −16% GPU time.**

`Host` mode was tested at the same time and is *not* better than `DeviceShared` despite removing
the memcpy as well — it was rejected on the measurement, not on principle. That closes the
"re-test Host buffer-cache mode" item on the G-list.

#### What did not change, and why that matters

**Frame rate did not move: 28.63 FPS in every variant.** Neither did CPU (204.9% → 205.3% —
the memcpy still happens, only the blit encoding disappears). A 16% GPU reduction changing
nothing is itself the finding: at 15.6–18.5 ms of GPU time against a ~35 ms wall-clock frame,
**the GPU is at roughly 50% duty cycle and is not what caps this scene.** The earlier
"GPU-bound / over budget" conclusion came from dividing by a 16.67 ms budget for a 30 FPS title.

So this change buys headroom and power, not frame rate, in the shrine. It should be re-measured
somewhere actually GPU-bound — the open world outside the Great Plateau is the obvious candidate
and is the scene that motivated using BotW at all.

#### Consequences for the rest of the graphics list

- The memoryless/`DontCare` work is **still blocked**, but no longer behind the deferred copy.
  ~13 same-FBO splits per frame remain (streamout copy, texture readback, and the rest); the
  depth textures are still written by up to 28 passes each, so memoryless remains unavailable.
- **The trade this change makes is an ordering guarantee.** A staging blit is ordered on the GPU
  timeline, so a draw that is already encoded still reads the old contents. A memcpy into shared
  storage lands immediately, so an in-flight draw may observe the new data one draw early. No
  visual difference appeared across ~8000 frames of BotW (two captures of the same static scene
  differ by 0.88% of pixels, all of it drifting dust motes), and Cemu already shipped this mode
  for Wind Waker HD — but a title that shows artifacts can be pinned back to `device private`
  through its game profile.

#### Method note

Two things made this tractable, and both are worth reusing:

- **Attribute, don't guess.** One `backtrace()` at every render-pass teardown, bucketed by stack
  and symbolized at report time, answered in a single run a question that two prior rounds of
  reasoning had gotten backwards. The cost is ~0.5% CPU in a diagnostic build.
- **Watch for state that is never cleared.** The first classifier used `m_state.m_textures[]` to
  test "is dst bound as a sampled texture". That array is only ever overwritten, never reset per
  pass, so it reports textures bound at any point in the past — the resulting figure was an upper
  bound, not a measurement. The FBO-pointer comparison had no such problem, which is why it is
  the number the conclusion rests on.

---

### Instrumenting three open questions at once — measured 2026-07-30

One build, one BotW run, three answers. Every counter added is gated on the telemetry area mask,
and the A/B against the same scene confirmed the instrumentation is free: frame ms median
33.27 → 33.27, p99 49.95 → 49.95, fps median 30.06 → 30.06, GPU busy 17.65 → 17.58 ms.

**1. `GX2SetAlphaToMaskReg` — a decisive negative, and the renderer work is retired unbuilt.**
It was 91.7% of all unresolved-import volume (2,757,074 of 3,005,784 calls), which is why it looked
like the biggest compatibility gap in the tree. Implementing the export and counting *draws with
`ALPHA_TO_MASK_ENABLE` set* — rather than calls to the setter — gives **0 across 9,181 frames**.
BotW calls it ~300 times a frame and never enables it. The MSL ordered-dither path, and the
pipeline-cache invalidation it would have forced on every user, are not needed. Details in
`../hardware/09-accuracy-gap-register.md` §4.2.

**2. A flat per-HLE-call cycle charge is impossible.** The counterfactual counter
(`cpu.hle_would_charge_cycles`, summed but never applied) says a flat 500/call would be
**25,103,500 cycles/frame against 15,384,240 actually retired — 163%**. The histogram shows the
volume sits in `OSFastMutex_Lock/Unlock`, `OSGetCoreId`, `OSBlockMove` and GX2 register setters,
whose real cost is tens of cycles, while the calls where the divergence matters (`FSReadFile`) are
rare. Any charge must be tiered by function.

**3. The renderer is holding uncommitted work essentially whenever the CP is idle.**
`MetalRenderer::NotifyLatteCommandProcessorIdle()` has a commented-out body and is called from
exactly the two places the command processor blocks. Measured per frame:

| | |
|---|---|
| idle notifications | 129,575 |
| **of which with recorded drawcalls pending** | **129,377 (99.85%)** |
| mean drawcalls pending at notification | 94.7 |
| command buffers committed per frame | 6 |

So the recorded draws sit unsubmitted for the whole stall. This is the live lead. Two caveats
before acting: the notification fires 129k times a frame (it is inside the ring-starvation spin, so
this counts spins, not distinct idle events), and **`m_commitOnIdle` does not exist** — the
commented-out line references a flag that was never added, so reviving it means designing the
throttle, not uncommenting a line.

**4. Two suspected blind spots are empty.** `IT_MEM_SEMAPHORE` had no instrumentation at all and
`IT_HLE_WAIT_FOR_FLIP` was timed but never counted. Both measure **exactly zero** in BotW —
`gpu.cp_semaphore_waits`, `gpu.cp_semaphore_spins` and `gpu.wait_flip_spins` are 0/frame. They are
not hiding time; strike them off.

**5. The fence-stall guest snapshot did not discriminate — a probe-design failure worth recording.**
Sampling `__currentCoreThread[0..2]` at stall entry finds all three cores with no thread scheduled
in ~95% of stalls (8,585 / 8,629 / 8,042 of 9,010). That looks damning until you compare it against
the cores' *baseline* idle rate, which is already ~96% (`cpu.coreN_idle_ns` ≈ 32.1 ms of a 33.3 ms
frame). **A sample taken at random would have found the same thing**, so the probe shows only that
the cores are idle during the stall as they are everywhere else. It needed a control sample at a
non-stall moment and did not have one. What it does yield is the list of threads ever caught
running — `RadarMgr` (678), `Default Core 1` (138), `GameScen TaskMgr` (129), `DecompThread` (121) —
and confirmation that the one stalling fence is still `fence@1046d420`, 9,010 stalls in 9,015 waits.

### The commit-on-idle hypothesis is refuted — measured 2026-07-30

The instrumentation above found the renderer holding uncommitted draws in 99.85% of
command-processor idle notifications and proposed the obvious chain: *the CP waits for the guest,
the guest waits for the GPU, and the GPU has no work because the CP never committed it.* If that
were true, submitting at stall entry would shorten the stall.

Implemented behind `--commit-on-fence-stall`, off by default. `NotifyLatteCommandProcessorIdle`
now takes a reason, because the two call sites are nothing alike: `RingStarvation` fires ~129,000
times a frame inside a park-and-recheck loop (committing there would mint a command buffer per
spin), while `FenceStall` fires once and means the guest will not proceed until the GPU retires
something. Only the second commits.

n=3 control, n=2 treatment, same scene, same binary. **Gameplay frames only** — see the correction
below; the first version of this table was computed over whole files and was wrong.

| | control | `--commit-on-fence-stall` |
|---|---|---|
| frame ms median | 49.89 · 49.89 · 49.90 | 49.90 · 49.90 |
| fps median | 20.04 ×3 | 20.04 ×2 |
| **`cp_fence` ms** | **14.95 · 15.13 · 15.41** | **15.50 · 15.54** |
| GPU busy ms | 18.81 · 18.85 · 18.98 | 19.07 · 19.36 |
| idle notifications holding work | 139,491 | 20,802 (−85%) |

**The mechanism works and the hypothesis is wrong.** Work held at idle drops 85%, one extra command
buffer is submitted per frame, and **nothing else moves at all** — frame time, fps, `cp_fence` and
GPU busy are all flat. The guest's 15.4 ms wait is not for GPU work the renderer was withholding.
That is the third time here that freeing something the command processor was blocked behind has
changed nothing: parking the fence spin (`5933733`) freed 40 points of CPU, `DeviceShared` cut GPU
time 16%, and the frame rate has not moved once.

Kept **off by default**, now on stronger grounds than before: it has no measured benefit at all.

#### Correction: the "−6.2% GPU busy" side effect was an artifact, not a result

The first version of this section reported GPU busy dropping 17.62 → 16.52 ms and called it real
because the ranges did not overlap. **They did not overlap because the two groups had different
menu-to-gameplay frame ratios**, not because the treatment did anything. `drive-botw.sh` spends its
first ~4,000 frames in the title and save menus, which are 113 draws and 4.2 ms of GPU work against
gameplay's 3,516 and 19.0 ms. A median over the whole file lands wherever that ratio falls.
Restricted to gameplay the effect is **+0.5%**, i.e. nothing.

Every whole-file median quoted in this repo for BotW is suspect for the same reason. The fix is in
the tool: `testing/telemetry-report.py` now segments a run by draw load, prints the phases, and
analyses the longest one unless told `--all`.

**Where the fence chain goes next.** Not into the renderer. The guest is genuinely waiting, its
cores are 18% busy, and the largest block reason is `cpu.block.sleep` at 115/frame. The next probe
should ask what the fence-owning thread is sleeping *on*, and it needs the control sample the
stall-entry snapshot lacked.

### The menu waits on the fence exactly as long as the open world does

Splitting runs by phase produced the clearest fact yet about `cp_fence`, and it was invisible while
whole-file medians blended the two:

| | menu | gameplay |
|---|---|---|
| frames per run | ~4,050 | ~4,750 |
| draws/frame | **113** | **3,516** |
| GPU busy/frame | **4.2 ms** | **19.0 ms** |
| frame | 33.27 ms (30.06 fps, 2 vsync) | 49.90 ms (20.04 fps, 3 vsync) |
| `cp_idle` | 16.61 ms (49.9%) | 19.91 ms (39.9%) |
| **`cp_fence`** | **15.25 ms (45.8%)** | **15.41 ms (30.9%)** |
| work (frame − waits) | **1.40 ms (4.2%)** | 13.91 ms (27.9%) |

Both decompositions close to within 1%.

**The command processor waits the same ~15.3 ms on a guest fence whether the game is drawing 113
objects or 3,516.** A 31× swing in draw load and a 4.5× swing in GPU time move it by 1%. That is
not a work dependency of any kind — not on the guest, not on the GPU. A wait whose duration is
independent of everything around it is a **timer**, and the next probe should test that directly
rather than looking for something for it to be waiting on.

Two supporting observations:

- In the menu the emulator does **1.4 ms of work in a 33.3 ms frame** and spends the other 31.9 ms
  in the two waits. Whatever holds the menu at 30 fps, it is not work.
- In gameplay the work (13.91 ms) fits inside **one** 16.68 ms vsync period with 16.6% headroom, and
  GPU busy (18.98 ms) needs **two**. Yet the frame takes **three**. The unexplained third period is
  ~15 ms, which is the size of `cp_fence`. That is a correlation and not yet a cause, but it is the
  first quantitative reason to think the fence is what separates 20 fps from 30 here — the
  commit-on-idle experiment ruled out one explanation for the fence, not the fence itself.

### `cp_fence` solved: it is the vsync timer, and it is correct behaviour

The previous section argued from a constant duration that the fence wait had to be a timer.
Measured directly, per stall:

| | menu | gameplay |
|---|---|---|
| `cp_fence` | 15.24 ms | 15.58 ms |
| vsync signals during the stall | **1** | **1** |
| flip events during the stall | **1** | **1** |
| **time from that vsync to the fence coming free** | — | **0.01 ms** (p99 0.03) |

**The fence is released ten microseconds after a vsync signal, every frame, in both phases.**

The chain, now fully closed:

1. The command processor hits `IT_WAIT_REG_MEM` and waits on a guest-written fence in MEM2.
2. The guest thread that writes it is parked in `GX2WaitForFlip` — `cpu.block.gx2_flip` and
   `cpu.block.gx2_vsync` are ~1/frame each.
3. Emulated vsync is a **polled software timer**: `LatteTiming_HandleTimedVsync` fires when
   `now >= timer_nextVSync` and advances by one 16.68 ms period. Host-driven vsync is a stub on
   this fork (`LatteTiming_EnableHostDrivenVSync` has an empty body).
4. That poll is called **from inside the fence wait loop itself**. When it fires,
   `__GX2NotifyEvent(FLIP)` wakes the guest thread, which writes the fence, and the CP proceeds.

So `cp_fence` measures *time until the next emulated vsync*. It is frame pacing working, not a
stall to be recovered, and **the whole line of investigation is closed**. In hindsight it explains
every negative result along the way: parking the spin (`5933733`) freed 40 points of CPU and moved
nothing, and committing work early moved nothing, because the guest was never waiting for the GPU.

One flip per vsync also pins `swapInterval = 1`, so the display grid is 16.68 ms and a 49.90 ms
frame is the title taking **three** grid slots.

**The remaining term is `cp_idle`, and it is now the largest:**

| gameplay, per frame | |
|---|---|
| frame | 49.90 ms |
| `cp_idle` — Latte thread waiting for guest commands | **20.03 ms (40%)** |
| `cp_fence` — waiting for vsync (solved, correct) | 15.58 ms (31%) |
| work | 14.09 ms (28%) |
| GPU busy (async) | 19.20 ms |

`cp_idle` is not a timer — it is the command processor genuinely out of work while the guest
cores sit ~18% busy. Twenty milliseconds a frame of the GPU pipeline waiting for a CPU that is
mostly idle is the next thing to explain, and unlike the fence it has no innocent explanation yet.

### `cp_idle` and the GPU's 33 ms of dead air

With the fence explained, `cp_idle` is the largest remaining term — 20.0 ms/frame of the command
processor with an empty ring while the guest cores sit 18% busy. Measuring the GPU side of the same
question (`gpu.gap_ns`: summed time between one command buffer ending and the next starting, which
is real idle because the buffers are serialised on an `MTLEvent`):

| gameplay, per frame | |
|---|---|
| frame | 49.90 ms (exactly 3 × 16.63 ms vsync, p99 49.97) |
| **GPU busy** | **19.37 ms** |
| **GPU gap** | **32.73 ms** |
| gaps observed | **2** |
| gaps over 4 ms | **2** |
| command buffers | 7 |

**The GPU is idle two thirds of every frame, and not in scattered stalls — in exactly two blocks of
roughly 16.4 ms, one vsync period each.** Five of the six inter-buffer transitions are back-to-back;
the GPU runs its seven buffers in a few contiguous bursts and then stops dead twice.

What this says about the 20-vs-30 fps question:

- GPU work is 19.37 ms. On its own that needs **two** 16.63 ms slots, not three.
- Latte-thread work is 14.03 ms. On its own that needs **one**.
- Fully overlapped the frame would be ~19 ms → 2 slots → 30 fps. Fully serialised it is
  19.37 + 14.03 = **33.4 ms**, which misses a 33.27 ms two-slot deadline by 0.1 ms and slips to
  three. **The frame is sitting exactly on the boundary, and the thing tipping it over is that CPU
  and GPU are not overlapping.**

That is the first quantitative account of why this scene is 20 fps rather than 30, and it points at
pipelining rather than at any individual cost. Two structural candidates, neither yet tested:

- `GetCommandBuffer` does `encodeWait(m_event, m_eventValue)` on every buffer, so command buffer
  N+1 cannot begin on the GPU until N has fully completed. Nothing overlaps by construction.
- The command processor only ever gets 7 buffers' worth of work per frame and spends 20 ms waiting
  for the ring, so even without the event chain there would be little to overlap.

**Attribution caveat, stated because it bounds the conclusion:** a completed buffer is credited to
the frame it *completed* in, so a gap spanning a frame boundary lands in the later frame. One of
the two gaps is plausibly that inter-frame idle, which would be benign. Distinguishing them needs a
per-gap timeline rather than per-frame sums, and that has not been done.

### The 20-vs-30 fps question is answered, and it is not the renderer

Instrumented the frame's *critical path* — first `GetCommandBuffer()` of a frame to that frame's
presenting buffer leaving the GPU — rather than continuing to infer it from GPU busy and Latte work
measured separately. That one counter settles the whole thing:

| | `GX2DrawdoneSync` **on** (default, n=3) | **off** (n=2) |
|---|---|---|
| frame ms median / p99 | 49.90 / 49.98 | **33.27 / 33.35** |
| **fps** | **20.04** | **30.06** |
| `gpu.frame_critical_path_ns` | **35.25 ms** | **18.64 ms** |
| `gpu.gap_intraframe_ns` | **16.39 ms** | **0.25 ms** |
| `gpu.sync_async_ns` | 6.78 ms | 0 |
| GPU busy | 19.33 ms | 19.41 ms |
| command buffers/frame | 7 | 7 |
| `gpu.queue_latency_ns` | 14.29 ms | 14.69 ms |

Both sides are deterministic to two decimal places and the ranges do not come close to overlapping.

**The critical path decides the vsync slot count, exactly.** 35.25 ms exceeds the two-slot deadline
(33.27 ms) → three slots → 49.90 ms. 18.64 ms exceeds one slot (16.63 ms) → two slots → 33.27 ms.
The model this investigation was built on is confirmed — but the term that overruns it is not the one
anybody had been looking at.

**What it is.** `GX2DrawDone` submits `IT_HLE_SYNC_ASYNC_OPERATIONS`, whose handler
(`LatteCommandProcessor.cpp:1872`) force-finishes texture readbacks and occlusion queries with bare
`waitUntilCompleted()` calls **on the Latte thread**. BotW emits it **twice per frame**. Measured
cost **6.78 ms**, of which **6.75 ms is readback force-finish** — `gpu.occlusion_flush_ns` is
**0.00 ms**, so occlusion queries are free and the entire cost is
`LatteTextureReadbackInfoMtl::ForceFinish` (`LatteTextureReadbackMtl.cpp:44`). Removing it collapses
the intra-frame GPU gap from 16.39 ms to 0.25 ms — a 98.6% reduction — because the drain was what
kept the GPU idle in the middle of every frame.

**What it is not.** Two Metal-side hypotheses are refuted directly by this run rather than argued
away: `gpu.queue_latency_ns` (14.29 → 14.69 ms) and `gpu.command_buffers` (7 → 7) do not move.
So neither the `MTLEvent` serialisation chain nor the commit threshold is implicated, and Steps 2–5
of the plan this work was following are aimed at the wrong subsystem.

**This is not a bug — it is a documented tradeoff whose price had never been measured here.**
`GX2DrawdoneSync` is a user-facing checkbox, "Full sync at GX2DrawDone()", tooltip *"more accurate
behavior, but may cause lower performance"* (`GeneralSettings2.cpp:409-410`), default **on**. The
finding is that on this fork's target hardware it costs BotW **exactly one third of its frame rate**,
and that the cost is entirely one call site.

**The right next step is therefore not to flip the default.** The drain force-finishes *every*
in-flight readback synchronously, whether or not the guest is about to read any of them. A surgical
version — wait only for readbacks whose results have actually been requested, or let the existing
non-forcing `LatteTextureReadback_UpdateFinishedTransfers(false)` sweep retire them — would keep the
accuracy the checkbox promises and recover most of the 6.75 ms. That is the work to scope next.

Also worth recording: `gpu.queue_latency_ns` is **14.29 ms/frame** in both arms. That is the hard
ceiling on anything a change to command-buffer ordering could recover, and it is large — so the
overlap question is not closed, merely demoted. It should be re-measured *after* the readback drain
is dealt with, against the new 33.27 ms frame, where the deadline arithmetic is completely different.

### The readback drain is not surgically improvable — measured

The obvious follow-up to the `GX2DrawDone` finding was that the drain is *indiscriminate*: it calls
`LatteTextureReadback_Update(true)` to force-start delayed transfers and then immediately blocks on
them, which would be the worst possible arrangement — start a GPU copy, wait for it with zero
runway. Two counters test that directly:

| | per frame |
|---|---|
| `gpu.readback_forcestart` (transfers the forced update actually started) | **0** |
| `gpu.readback_age_at_wait_ns` (how long the transfer had already run) | **14.19 ms** |
| `gpu.readback_forcefinish` / `_ns` | 2 / 6.72 ms |

**The 5-drawcall delay holds nothing back.** Every readback blocked on at `GX2DrawDone` was already
in flight, started on average ~7 ms earlier, and still had not completed. So we are not waiting for a
texture copy — **we are waiting for the GPU to catch up**, which is precisely what `GX2DrawDone` is
specified to do.

That kills "make the drain surgical" as stated. There is no subset of the wait that can be skipped
while preserving the semantics: the call means *the GPU has retired everything*, and the GPU is
roughly one pipeline depth behind when it is asked.

What remains genuinely improvable is narrower and should not be confused with the above:

- ~~The readback blit is appended to the **current** command buffer, so waiting for it means waiting
  for every draw encoded ahead of it — up to ~500. A blit in its **own** command buffer would need
  only its true dependency.~~ **Refuted by measurement — see "the readbacks are real" below.** The
  draws actually ahead of the blit are ~28, not ~500 (`gpu.readback_draws_ahead`), and giving it an
  unchained buffer of its own moved nothing at n=3 per arm.
- After the block the pipeline is empty and has to refill, which is where the 16.4 ms of
  `gap_intraframe` comes from. **That refill is not inherent to the semantics** and is the larger
  number of the two — and it is now the only one of the pair still standing.

Neither is a small change, and neither is justified by the evidence gathered so far. The honest
status is: the accuracy option costs what it costs, the cost is now measured and documented
(`docs/hardware/09` gap 2.7), and whether it stays on by default is a product decision rather than an
engineering one.

### The MTLEvent wrap was real, and fixing it corrects every `gpu.busy_ns` ever recorded

`EVENT_VALUE_WRAP` was 4096 and `m_eventValue` stepped `(v + 1) % 4096`, sending the signalled value
**backwards** every 4096 command buffers. `MTLEvent` requires monotonically increasing values. At 7
buffers/frame and 20 fps that is a wrap **every 29 seconds** — ten times in a single telemetry run,
not a once-in-a-blue-moon edge case. Replaced with a monotonic `uint64`.

The fix is strictly a correctness change, but it moved a counter, and the direction is the
interesting part. n=4 wrapping, n=2 monotonic, gameplay phase, ranges non-overlapping:

| | wrapping | monotonic |
|---|---|---|
| frame ms / fps | 49.90 / 20.04 | **49.90 / 20.04** |
| `gpu.frame_critical_path_ns` | 35.24 – 35.25 | **35.23** |
| **`gpu.busy_ns`** | 19.14 – 19.37 | **16.74 – 16.92** |
| `gpu.queue_latency_ns` | 14.13 – 14.28 | **16.27 – 16.56** |

**`gpu.busy_ns` sums `GPUEndTime − GPUStartTime` per command buffer, so it double-counts whenever
buffers overlap.** GPU busy dropping 13% under *stricter* ordering, with identical draw counts, is
the signature of overlap having been happening before and being counted twice. Queue latency rising
15% is the same fact from the other side: strictly ordered buffers wait longer to start.

Two consequences.

**1. It is independent confirmation that overlap does not help.** The wrap had been letting command
buffers overlap for most of every run, and frame time and critical path are identical to two decimal
places with it on and off. That is a stronger refutation of the "remove the serialisation chain"
hypothesis than the queue-latency argument used earlier, because it is a direct observation of
overlap occurring and buying nothing.

**2. Every `gpu.busy_ns` figure recorded before this fix is inflated**, including the 19.3 ms in the
`GX2DrawDone` section above and the 18.7 ms in the older Korok Forest baselines. The true serialised
figure for that scene is **16.8 ms**. The inflation is not a constant: the first wrap only happens
~29 s into a run, so short traces are unaffected and long ones are affected more — which makes old
numbers inconsistently wrong rather than uniformly wrong. Conclusions that rested on *frame time*
(the `GX2DrawdoneSync` result, the phase-split correction) are unaffected; conclusions that rested on
GPU busy in absolute terms should be re-derived.

Also fixed alongside: `m_metalBufferCacheMode` had no initialiser but was read by
`NeedsReducedLatency()` seven lines after the memory manager was constructed, to choose the commit
threshold — indeterminate memory deciding submission cadence. It now has an initialiser, and the
threshold is set explicitly to the 196 that behaviour has always shown, rather than being switched to
64 under cover of a correctness fix. There is no evidence for 64: command buffer count does not move
frame rate in this scene.

### The ring-buffer `wfe` park is a no-op, and `sched_yield` is 36% of a core

`612d064` replaced the command processor's ring-starvation spin with an `ldxr`/`wfe` park and
recorded *"a pass takes ~1269 ns instead of ~42 ns — about 30x fewer idle iterations"*. Measured
inside BotW the same loop takes **150 ns/pass**, so something was wrong with either the claim or the
park. Full anatomy, gameplay phase:

| | per frame | per pass | share of `cp_idle` |
|---|---|---|---|
| ring-starvation passes | **134,928** (2.70 M/s) | | |
| `cp_idle` total | 20.20 ms | 149.7 ns | 100% |
| **`sched_yield`** | **17.95 ms** | **133.0 ns** | **88.9%** |
| vsync + async checks | 0.67 ms | 4.9 ns | 3.3% |
| **`wfe` (the "park")** | **0.09 ms** | **0.7 ns** | **0.4%** |
| unattributed | 1.49 ms | 11.1 ns | 7.4% |

**The `wfe` returns in 0.7 ns — it is a no-op.** `gpu.rb_wait_early_out` is 0, so it is executed on
essentially every pass and simply does not wait.

The claim in `612d064` is not wrong about the instruction, it is wrong about the context.
`tools/probes/wfe_under_load.c` reproduces the original conditions and confirms **~1250 ns, and that
figure does not move with 0, 1, 3 or 7 busy sibling threads** — so machine load alone is not the
explanation. What differs in the emulator is that something clears the exclusive monitor or sets the
event register continuously; ARM's WFE is defined to return immediately when the event register is
already set. The producer writing the ring is the obvious candidate, the reservation granule covering
neighbouring globals another. **Not yet pinned down, and it should be before anything is built on
`wfe` again anywhere in this codebase.**

The practical consequence is independent of the cause: the loop is a spin, and **`sched_yield` alone
is 17.95 ms of every 49.90 ms frame — 36% of one core** — running 2.7 million times a second.

**The fix is a genuine trade-off, not an obvious win, which is why nothing has been changed yet.**
Removing the yield makes it a hotter spin, not a cooler one — the syscall is what currently paces the
loop, so the pass rate would rise to fill the gap. Making the wait actually sleep (a short
`nanosleep` after N consecutive failures, or a real blocking primitive) costs command-pickup latency,
which is precisely what `612d064` was careful to preserve (it measured wakeup at 42–208 ns). Given
`cp_idle` is 40% of the frame, tens of microseconds of pickup latency is likely irrelevant — but
"likely" is not a measurement, and the precedent here is that freeing CPU in the command processor
has never once moved the frame rate (`5933733`: 40 points of CPU, zero fps).

So this is a **power and thermal** item on a machine already running three guest cores and the shader
pools on four P-cores, not a frame-rate one, and it should be scheduled and measured as such.

### Command buffers now have a timeline instead of a pointer identity

The renderer identified a submission by its `MTL::CommandBuffer*`, which is not a safe key for two
independent reasons. `ProcessFinishedCommandBuffers` `release()`s the buffer while
`m_currentCommandBuffer` still holds the same pointer, so `GetCurrentCommandBuffer()` can hand out a
dangling one — and Metal is free to return that address again for a later buffer, so a stale entry
would match a live submission. `GetAndRetainCurrentCommandBufferIfNotCompleted` papered over the
first case with `m_executingCommandBuffers.size() == 0`, an inference that is only sound while the
`MTLEvent` chain guarantees the newest buffer retires last.

Replaced with the scheme the deleted Vulkan backend used: `m_submittedCount` / `m_retiredCount`, and
ids in `[0, m_retiredCount)` have completed.

- `ProcessFinishedCommandBuffers` now retires the **in-order prefix** and stops at the first
  unfinished buffer. The old loop erased completed buffers from the middle of the vector, which
  destroyed the very property a ring allocator needs.
- `CleanupBuffer` takes a **watermark** rather than "some buffer finished". The ring allocator popped
  sync points only from the front by pointer equality, so anything retiring out of order stranded its
  sync point permanently and leaked a 32 MB ring at a time; the heap allocator's release queue became
  a `std::map` so the completed prefix is erased in one sweep.
- Occlusion queries hold an id, so `LatteQueryObjectMtl` no longer retains a command buffer purely to
  ask whether it finished, and its destructor is gone.
- `ProcessFinishedCommandBuffers` is now also pumped from `SwapBuffers`. It previously ran only from
  `CommitCommandBuffer` and `occlusionQuery_updateState`, so **an emulator that stopped committing —
  paused, or a menu with almost no draws — never reclaimed staging memory at all.**

Texture readback is deliberately **not** converted and keeps its retained pointer. An id would make
`IsFinished()` depend on the retire pump having run, where the pointer polls Metal directly, and a
readback that merely *looks* unfinished costs a forced blocking wait at the next `GX2DrawDone` — the
most expensive thing in the frame. The pointer is safe there because `GetBlitCommandEncoder` has
already ensured a live uncommitted buffer.

Verified behaviour-neutral over 9,921 frames: frame 49.90 ms, 20.04 fps, GPU busy 16.74 → 16.76 ms,
7 command buffers, draws and passes unchanged, screenshot identical, no crash.

New counter `gpu.staging_rings_created` is the leak watch, and it is worth knowing how to read it: it
totalled **6** for the run, which looks alarming until you see *when* — one at startup and five
across frames 4028–4031, the menu→gameplay transition where the workload jumps from 113 draws to
3,375. **Flat across all 5,712 gameplay frames afterwards**, and RSS held at ~1.39 GB with no
staircase. Growth to a working set is not a leak; a climb during steady state would be.

### What BotW actually reads back: 21 KB of float arrays, delivered by draining the pipeline

`enableReadback` is set for **any** `TM_LINEAR_ALIGNED` texture (`LatteTexture.cpp:1311`) on the
theory that the CPU can read a linear surface directly. The hypothesis worth testing was that BotW's
linear targets are never actually read, in which case the `GX2DrawDone` drain — 6.75 ms/frame, the
whole 20-vs-30 fps gap — would be paid for nothing.

44 distinct textures are mirrored over a run, but almost all are one-off asset loads during the
menu→gameplay transition. **Steady state is six tiny surfaces**, each queued once every ~2 frames:

| physAddr | size | format | bytes | queued |
|---|---|---|---|---|
| `3cdd6500`, `3cdd8600` | 64×8 | `R32_G32_B32_A32_FLOAT` | 8 KB each | 2,483 |
| `3d110b00`, `3d112700` | 64×3 | `R16_G16_B16_A16_FLOAT` | 1.5 KB each | 2,483 |
| `3d10d900`, `3d10de00` | 64×1 | `R32_G32_B32_A32_FLOAT` | 1 KB each | 1,649 |

**~21 KB in total.** These are not images — 64-wide arrays of RGBA floats are a GPU-computed data
table being handed back to the CPU, and a game does not build one of those by accident. So the
hypothesis is **refuted: the readbacks are real and the game wants them.** Skipping them is not
available.

What the finding does establish is where the cost actually is. The transfer is 21 KB; measured
`readback_age_at_wait_ns` is 14.19 ms and `readback_forcestart` is 0, so the copy is neither slow nor
late. **The entire 6.75 ms is pipeline-drain latency to deliver 21 KB**, because the blit is encoded
at the end of a command buffer holding a frame's worth of unrelated draws, and a command buffer's
encoders execute in order.

That pointed at a much narrower change than the one already refuted — and **it was implemented,
measured, and came up empty. Do not re-raise it.**

The idea: removing the `MTLEvent` chain globally does nothing (measured twice), but *exempting the
readback's own command buffer* would leave the blit ordered against its true dependency by Metal's
automatic hazard tracking — it reads a texture a render pass wrote, and every resource here is
default-tracked — while freeing it from ~500 unrelated draws.

Two counters killed it. First the premise: `gpu.readback_draws_ahead`, added for this, records the
draw passes already in the buffer the blit joins. It is **169 per frame summed over all readbacks,
~28 each**, against 3,516 draws/frame — so the "end of a command buffer holding a frame's worth of
unrelated draws" framing above overstates the intra-buffer component by more than an order of
magnitude. Then the experiment itself, `--readback-unchained`, n=3 per arm, gameplay phase:

| metric | control | treatment | |
|---|---|---|---|
| frame ms | 49.90 – 49.90 | 49.89 – 49.90 | overlap |
| fps | 20.04 – 20.04 | 20.04 – 20.04 | overlap |
| `frame_critical_path_ns` | 35.13 – 35.22 | 35.10 – 35.23 | overlap |
| `readback_forcefinish_ns` | 6.21 – 6.34 | 6.14 – 6.27 | overlap |
| `sync_async_ns` | 6.23 – 6.37 | 6.17 – 6.30 | overlap |
| `busy_ns` | 16.44 – 16.51 | 16.43 – 16.53 | overlap |
| `command_buffers` | 7 – 7 | 9 – 9 | **separated** |

Every performance range overlaps. The only separated metric is the buffer count, which just proves
the arm was live (the exempt buffers were being minted). The mechanism was **reverted** — it rested
on an unverified assumption about Metal's cross-command-buffer hazard tracking, and carrying that
risk behind a default-off flag buys nothing. `gpu.readback_draws_ahead` was kept.

The same batch gives the noise floor for this subsystem, which matters for anyone measuring here:
**control vs control moves `readback_forcefinish_ns` by −2.1%**, while frame time, critical path and
`queue_latency_ns` are stable to 0.0% and `command_buffers` is exact. A few percent on the drain
counters is nothing; only a same-arm control can tell you which.

So all three surgical avenues are now closed — start delay (`forcestart` = 0), intra-buffer position
(~28 draws), and inter-buffer ordering (this experiment). The wait is the GPU genuinely still having
~6 ms of work outstanding when `GX2DrawDone` asks. **Only reducing GPU work can shorten it.**

### The 33 ms frame decomposed — and 60 fps is out of reach without cutting GPU work

Every decomposition in this document was taken at 49.90 ms, a frame shape the drain creates. With
`GX2DrawdoneSync` off, gameplay phase, n=1:

| | 49.90 ms frame | **33.27 ms frame** |
|---|---|---|
| fps | 20.04 | **30.06** |
| `cp_idle` | 20.05 ms (40%) | **19.93 ms (60%)** |
| `cp_fence` (vsync wait) | 15.54 ms | 5.98 ms |
| work (frame − waits) | 14.20 ms | **7.37 ms** |
| GPU busy (concurrent) | 16.85 ms | **16.69 ms** |
| critical path | 35.23 ms | **18.63 ms** |
| gap: inter-frame / intra-frame | 16.73 / 16.40 | 16.25 / **0.33** |

Three things fall out.

**1. `cp_idle` is a constant, not a proportion.** It is ~20 ms in both configurations while the frame
shrinks by a third. The command processor waits the same absolute time for guest commands either way,
so it is now **60% of the frame** and the largest single term. It is also not obviously waste — it
overlaps GPU execution.

**2. 60 fps is arithmetically unavailable here.** One vsync slot is 16.63 ms and **GPU busy alone is
16.69 ms**, already over. The CPU side has 55.8% headroom. So any further frame-rate work in this
scene has to reduce GPU work — 203 render passes with `LoadActionLoad` + `StoreActionStore` on every
attachment is the standing candidate — and nothing else will do.

**3. The intra-frame gap really was entirely the drain**, 16.40 → 0.33 ms. What remains is the
inter-frame gap, which is the GPU finishing early and waiting for vsync: correct behaviour.

Note `gpu.gap_count` now reads 7 rather than the 2 recorded earlier. That is the in-order retirement
fix working: previously buffers retiring out of order let `m_lastGpuEndTime` run ahead and swallowed
most transitions, so the gap measurement itself was undercounting.


---

### The GPU is ALU- and texture-bound, not bandwidth-bound — the streamout work is cancelled

`Metal GPU Counters` capture, Korok Forest gameplay, 20.5 s, 179,221 samples per counter
(`testing/traces/step1-gpucounters.trace`, gitignored). Medians are meaningless here because the GPU
idles ~67% of the frame, so everything below is conditioned on **active** samples — those where some
limiter exceeds 5%, which is 42.1% of all samples and matches the ~33% duty cycle.

| limiter | mean % during active samples | share of samples where it is THE top limiter |
|---|---|---|
| **ALU** | **35.34** | **45.6%** |
| Texture Sample | 28.26 | 22.8% |
| Texture Filter | 22.27 | 4.4% |
| Texture Write | 15.31 | 13.3% |
| **GPU Last Level Cache** | **17.62** | **7.3%** |
| **MMU** | **13.15** | **5.5%** |
| Buffer Read / Write | 0.88 / 0.00 | ~0% |

GPU bandwidth during active samples: read 24.04 GB/s, write 17.22 GB/s, **combined 41.26 GB/s mean**
(p95 98.33). `Partial Renders Count` is **0 across every sample** — no tile-memory spills, so the
pass structure is not pathological in the way that would matter most.

**The memory path is the bottleneck in only 12.8% of active samples (LLC 7.3% + MMU 5.5%). ALU and
texture together are the bottleneck in 86.1%.**

The 804 MB/frame of attachment traffic is real and the figure is confirmed — 16.1 GB/s at 20.04 fps,
about 39% of measured active bandwidth. But eliminating the 300 MB/frame of same-FBO split waste
would relieve a subsystem that limits the GPU roughly an eighth of the time. **It cannot convert into
the 1.90 ms the deadline needs.**

Per the plan's own gate: report and stop rather than build it anyway. **Step 4 (the in-pass streamout
copy) is cancelled** — not because the mechanism would fail, but because the premise underneath it is
false.

**The other gate passed, and is recorded for whoever revisits this.** `gpu.streamout_read_fragment`
is **0** while the `gpu.streamout_read_vertex` control reads **1,136/frame**, so nothing in the
fragment stage reads a streamout range and a vertex-only intrapass barrier *would* be legal on Apple
silicon. If the ALU bottleneck is ever addressed and bandwidth becomes the limiter, the in-pass copy
is unblocked and this measurement is the evidence.

> The first version of that detector read zero on **both** counters. The scope was self-defeating:
> the streamout copy calls `GetBlitCommandEncoder()`, which ends the render pass, so recording a
> write also guaranteed the list was cleared before any read arrived — "read within the pass that
> wrote it" was unreachable by construction. The vertex control is the only reason this was caught
> rather than published as "nothing reads streamout". Scope is now frame-wide.

**Where GPU time actually goes, and what that means next.** Any further frame-rate work in this scene
has to reduce **shader cost**, not memory traffic: ALU work and texture sampling/filtering. That is a
different and harder project than anything attempted so far — it points at the MSL the Latte
decompiler emits, and at texture filtering settings, rather than at the Metal backend's structure.
Bandwidth-shaped optimisations are now closed by measurement alongside the ordering-shaped ones.


---

### The accuracy-for-speed levers, measured — and neither default is changed

Two user-facing settings trade correctness for frame rate. Recording what each actually costs so the
"leave it on" decision is informed rather than inherited. **Neither default is being changed here** —
that is a product call, not an engineering one.

| lever | default | state on this fork | measured effect | correctness cost |
|---|---|---|---|---|
| **`GX2DrawdoneSync`** | **on** | live, honoured | **20.04 → 30.06 fps** on Korok gameplay; critical path 35.25 → 18.64 ms; intra-frame GPU gap 16.39 → 0.25 ms. n=3 control / n=2 treatment | The guest asked for a full pipeline drain and does not get one. BotW reads back 21 KB of GPU-computed float arrays per frame that it genuinely consumes — see "what BotW actually reads back" |
| **Accurate barriers** | `true` | **not implemented** — dead code since `26e40a4` | **unavailable.** The fork already runs as if it were off, so its performance is banked in every number in this document | Already being paid: any effect sampling a render target while writing it reads undefined data. BotW lava/waterfall are the known cases |

Two consequences worth stating plainly.

**1. The headline 20-vs-30 fps gap is a single accuracy option, and it is the *only* lever that
crosses the deadline.** Every engineering attempt to recover the same 1.90 ms has failed on
measurement: the `MTLEvent` chain (twice), the commit threshold, commit-on-idle, a surgical readback
drain, the readback command-buffer exemption, and now the in-pass streamout copy. `GX2DrawdoneSync`
remains the one thing that works, and it works by declining to do what the guest asked.

**2. Implementing 3.1 will make things slower, and that is correct.** Splitting render passes for
self-dependency raises pass count — the very thing the bandwidth work was trying to reduce. It should
still be done: shipping wrong pixels quietly is worse than shipping fewer frames honestly. But it
should be done with the expectation that frame rate drops, and it makes the (currently cancelled)
streamout work more valuable, not less, by moving the GPU further toward the memory path.


---

### Re-measuring the Korok verdicts at HEAD — all four hold, and one counter turns out to soak

Twelve of the sixteen measured ledger entries were taken before one of the two commits that
invalidate earlier numbers wholesale — `491f4ef` (the phase split, before which whole-file medians
blended menu and gameplay) and `839466d` (the `MTLEvent` wrap, before which `gpu.busy_ns`
double-counted overlapping buffers). Rather than argue about which gate reached which counter, the
four load-bearing ones were re-run: **`cebe17f`, n=3, default configuration, Korok Forest, the
gameplay phase selected by `telemetry-report.py`'s own splitter** (4,652 / 4,674 / 4,666 frames).

| claim | as recorded | re-measured, n=3 |
|---|---|---|
| frame median / p99 | 49.90 / 49.97 ms | 49.90 / 49.98 – 50.00 ms |
| fps | 20.04 | 20.04 |
| draws / passes per frame | 3,480 / 201 | 3,517 – 3,528 / 202 – 203 |
| all three guest cores busy | 26.89 ms = 18% | 26.84 – 27.18 ms = 17.9 – 18.2% |
| `cp_idle` / `cp_fence` | 20.03 / 15.58 ms | 19.47 – 19.51 / 15.39 – 15.42 ms |
| HLE calls / thread switches | 52,330 / 3,143 | 51,974 – 53,276 / 3,138 – 3,142 |
| yield / quantum / blocked split | 79.4 / 10.5 / 10.0% | 79.4 – 79.5 / 10.4 – 10.5 / 10.0% |
| `hle_would_charge` ÷ retired | 163% | 163 – 164% |
| `gap_count` / `gap_large` | 7 / 2 | 7 / 2 |
| `frame_critical_path_ns` | 35.25 ms | 35.25 – 35.26 ms |
| **`gpu.busy_ns`** | **16.45 ms** | **17.12 – 17.17 ms** |

**Every verdict survives.** The blocked-thread breakdown reproduces exactly (sleep 52%, message
queue 24%, event 15%, semaphore 9%), and the three runs agree with each other to ±0.4% on counters
where the recorded value differs by 4%. That last fact is what makes the one discrepancy
interesting rather than dismissible.

**`gpu.busy_ns` soaks.** Slicing a single gameplay phase into sixths shows the *work* holding
still while the *time* climbs:

| slice | draws | passes | attach MB | GPU ms | drain ms | core1 ms | crit ms |
|---|---|---|---|---|---|---|---|
| 0 | 3,472 | 203 | 804.3 | 16.9 | 6.5 | 9.5 | 35.1 |
| 1 | 3,539 | 202 | 800.6 | 16.8 | 6.5 | 9.5 | 35.3 |
| 2 | 3,536 | 202 | 800.6 | 16.8 | 6.6 | 9.4 | 35.3 |
| 3 | 3,518 | 202 | 800.6 | 17.5 | 7.2 | 9.4 | 35.3 |
| 4 | 3,562 | 202 | 800.6 | 17.5 | 7.2 | 9.4 | 35.3 |
| 5 | 3,518 | 202 | 800.6 | 17.8 | 7.5 | 9.5 | 35.1 |

Identical draw count, identical pass count, identical attachment traffic, identical guest-CPU cost —
and 5% more GPU time by the end, with the readback drain up 15%. All three runs show it
(+3.6 / +4.7 / +4.2% first-quarter to last-quarter on `gpu.busy_ns`). The scene is not changing:
if BotW's day/night cycle were responsible, draw count or attachment traffic would move with it.
The GPU is doing the same work more slowly under sustained load.

Three consequences.

**1. The recorded 16.45 ms and today's 17.15 ms are the same measurement at different window
lengths**, not a regression. Nothing between those commits touches GPU work.

**2. `gpu.busy_ns` and `gpu.readback_forcefinish_ns` are only comparable between equal-length,
equally-positioned windows.** A shorter arm wins on those counters for free. This retroactively
explains the "`readback_forcefinish_ns` swings ±2% between identical runs" note recorded with the
readback-exemption experiment — that was not noise, and the same-arm control it recommends is now
doubly necessary.

**3. The vsync-pinned metrics hid it.** Frame time, fps and `frame_critical_path_ns` drift 0.0%
across the same window, because they are quantised to the flip. The headline numbers look
rock-steady while the counters underneath them walk, which is precisely the shape of a trap that
survives casual checking.

> Found while doing this: the `build` field in every telemetry header is stale. `CMakeLists.txt:15`
> captures the hash at cmake *configure* time, so a binary compiled from `cebe17f` reported
> `064d9a7`. `capture-scene.sh` fails the other way, stamping `baseline.tsv` from the repo's HEAD
> rather than the binary. Both are recorded in CLAUDE.md; the runs above were taken after an
> explicit reconfigure, which is why their headers read `cebe17f`.
