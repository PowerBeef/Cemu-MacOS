<!-- Generated during the Apple Silicon / macOS 26 fork research pass.
     Findings verified against the source tree at commit b8f2cf4 and the
     macOS 26.5.2 / Xcode 26.6 SDK on an Apple M2. See 00-master-plan.md. -->

# FOUNDATION / PLATFORM / PACKAGING — Implementation Plan

## Key verified facts that shape this plan

Three findings from probing this exact machine change several of your assumptions:

1. **`-mcpu=apple-m1` is a no-op here.** Apple clang 21 already defaults `-target-cpu` to `apple-m1` for `arm64-apple-macos`, with `+v8.5a +lse +aes +sha2 +dotprod +fullfp16 +complxnum +flagm +jsconv +pauth +rcpc +sb +ssbs`. Verified via `clang -### `. Adding `-mcpu` buys nothing. Do not add it.
2. **macOS 26 ships a *new* JIT write API.** `$(xcrun --show-sdk-path)/usr/include/pthread.h` has `pthread_jit_write_with_callback_np`, `PTHREAD_JIT_WRITE_ALLOW_CALLBACKS_NP`, `pthread_jit_write_freeze_callbacks_np`. Apple's doc (`/documentation/apple-silicon/porting-just-in-time-compilers-to-apple-silicon`) states that adopting `com.apple.security.cs.jit-write-allowlist` **disables `pthread_jit_write_protect_np()` entirely**. Do not adopt it — see §4.
3. **Apple's doc states: with hardened runtime + `allow-jit`, the app "can only create one memory region with the `MAP_JIT` flag set."** This is a hard architectural constraint and it single-handedly decides §4: you need one first-party JIT arena, not per-function allocations.

Machine confirmed: `hw.pagesize=16384`, `hw.memsize=8589934592`, `hw.perflevel0.logicalcpu=4`, `hw.perflevel1.logicalcpu=4`, macOS 26.5.2, clang 21, cmake 4.3.2, ninja 1.13.2. All 7 submodules uninitialized (`git submodule status` shows leading `-` on all).

---

# Phase 0 — Bootstrap to a first running arm64 binary

**Goal:** `cmake+ninja` produces a launchable `bin/Cemu_release` on this machine, with zero source changes beyond what's strictly blocking. Everything else is deferred.

### 0.1 Initialize submodules — but not all of them (S)

`.gitmodules` lists 7. You need 5:

| Submodule | Keep? | Why |
|---|---|---|
| `dependencies/vcpkg` | **yes** | dependency manager; also gets unshallowed by `CMakeLists.txt:24-40` |
| `dependencies/ZArchive` | **yes** | `.wua` container support |
| `dependencies/cubeb` | **yes** | only viable macOS audio backend |
| `dependencies/imgui` | **yes** | overlay |
| `dependencies/metal-cpp` | **yes** | Metal renderer |
| `dependencies/Vulkan-Headers` | **no** | deleted in Phase 1 |
| `dependencies/xbyak_aarch64` | **yes, initially** | needed to compile `BackendAArch64.cpp` at all. Replaced in Phase 3, kept as an assembler-only dependency. |

Command: `git submodule update --init --depth 1 dependencies/{vcpkg,ZArchive,cubeb,imgui,metal-cpp,xbyak_aarch64}` — note vcpkg is declared `shallow = false` and root CMake unshallows it anyway, so let it clone fully (~2 min).

**Verify:** `git submodule status` shows no leading `-` for those six.

### 0.2 Configure and build with renderers disabled except Metal (S)

Do **not** touch `MACOS_BUNDLE` yet — `MACOS_BUNDLE=ON` + `ENABLE_VULKAN=ON` hits the `FATAL_ERROR` at `src/CMakeLists.txt:127-135` because you have no MoltenVK. With `MACOS_BUNDLE=OFF` (the default), the non-bundle branch at `:198-210` only issues a `message(WARNING)`. But you also want Vulkan out of the picture immediately.

```
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_VULKAN=OFF -DENABLE_OPENGL=OFF -DENABLE_METAL=ON \
  -DENABLE_DISCORD_RPC=OFF -DMACOS_BUNDLE=OFF
cmake --build build
```

Two things will break, and both need a code fix, not a flag:

- **`find_package(glslang REQUIRED)` at `CMakeLists.txt:158`** is unconditional and `CemuCafe` links `${glslang_target}` unconditionally at `src/Cafe/CMakeLists.txt:658`. With `ENABLE_VULKAN=OFF` glslang is still fetched and linked. Leave it for Phase 0 (it builds, just wastes ~4 min of vcpkg time); remove in Phase 1.
- **`ENABLE_VULKAN=OFF` may not actually compile.** `src/Cafe/CMakeLists.txt`, `src/imgui/`, `src/gui/wxgui/CemuApp.cpp:367-369` (`InitializeGlobalVulkan()`), `src/Cafe/HW/Latte/Renderer/Renderer.cpp` all have `#ifdef ENABLE_VULKAN` guards of varying quality. Expect 5–20 compile errors. **Fix them by deletion, not by `#ifdef`** — you're deleting Vulkan in Phase 1 anyway, so just do the deletion now (see 1.1). This merges Phase 0 and the start of Phase 1, which is correct: there is no reason to make `ENABLE_VULKAN=OFF` work as an *option*.

**Revised ordering recommendation:** do §1.1 (renderer deletion) *before* attempting the first build. It removes more build surface than it adds work.

### 0.3 Blockers you will hit, pre-diagnosed

| Symptom | Cause | Fix |
|---|---|---|
| vcpkg builds `libusb` as a **dylib** | `dependencies/vcpkg_overlay_ports_mac/libusb/portfile.cmake` forces `VCPKG_LIBRARY_LINKAGE dynamic` | leave as-is in Phase 0; see §7.4 |
| `wxwidgets` 3.3.3 build takes 15–25 min | vcpkg source build | expected; enable a local binary cache: `export VCPKG_DEFAULT_BINARY_CACHE=$HOME/.cache/vcpkg` |
| Link error `_glslang…` | glslang linked unconditionally | Phase 1.2 |
| `cemu_assert` fires at startup on 16 KB pages | `MemMapperUnix` (§3) | Phase 2; app may *launch* fine and only fail on second title load |

**Verify Phase 0:** `./bin/Cemu_relwithdebinfo` opens the wx main window, the game list appears, Settings dialog opens. Do **not** expect a game to boot yet.

**Effort: S–M (1 day, mostly waiting on vcpkg).**

---

# Phase 1 — arm64-only + macOS 26 purge

Each sub-step ends buildable. Do them in this order.

### 1.1 Delete OpenGL and Vulkan (M)

Delete (`git rm -r`):
- `src/Cafe/HW/Latte/Renderer/OpenGL/` (19 files)
- `src/Cafe/HW/Latte/Renderer/Vulkan/` (32 files)
- `src/Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompilerEmitGLSL.cpp`, `…EmitGLSLAttrDecoder.cpp`, `…EmitGLSLHeader.hpp`
- `src/util/Zir/EmitterGLSL/`, `src/util/Zir/Passes/RegisterAllocatorForGLSL.cpp`
- `src/imgui/imgui_impl_vulkan.cpp/.h`, `src/imgui/imgui_impl_opengl3*`
- `dependencies/Vulkan-Headers` submodule (remove from `.gitmodules` and `.git/config`)

CMake edits:
- `CMakeLists.txt:113-123` — delete `ENABLE_OPENGL`/`ENABLE_VULKAN` options and the `ENABLE_*_DEFAULT` block; hard-set `add_compile_definitions(ENABLE_METAL)`.
- `CMakeLists.txt:186-197` — delete the Vulkan/OpenGL `find_package`/`include_directories` blocks.
- `src/CMakeLists.txt:17-25` — the `if (ENABLE_VULKAN)` branch collapses; `_XOPEN_SOURCE` survives here for now (see 1.6).
- `src/Cafe/CMakeLists.txt` — remove the Vulkan/OpenGL source lists and `${glslang_target}`.
- `src/CMakeLists.txt:126-135, 143-149` — the entire MoltenVK probe/copy dies. `src/CMakeLists.txt:190-206` (the non-bundle MoltenVK `find_library` + `INSTALL_RPATH "/usr/local/lib;/opt/homebrew/lib"`) dies too. **This is what unblocks `MACOS_BUNDLE=ON` on your machine.**

Source edits:
- `src/Cafe/HW/Latte/Renderer/Renderer.h/.cpp` — collapse the backend enum/factory to Metal only.
- `src/gui/wxgui/CemuApp.cpp:367-369` — delete `InitializeGlobalVulkan()`.
- `src/config/CemuConfig.h` — the `GraphicAPI` enum keeps only Metal; migrate old configs to Metal on load rather than asserting.
- `src/gui/wxgui/GeneralSettings2.cpp` — remove the graphics-API combobox (or reduce to a disabled label).

**Why:** ~50 source files and the single biggest build-time and dependency reduction available. It also removes the *only* thing blocking a bundle build on this machine.

**Verify:** `nm -u bin/Cemu_* | grep -ci -e vk -e gl` returns 0 (allowing for `MTLGL`-free output); app still launches.

### 1.2 Trim vcpkg dependencies (S)

`vcpkg.json`:
- Remove `glslang` from `dependencies` **and** from `overrides` (only consumer was `RendererShaderVk.cpp`/`VulkanRenderer.cpp` — verified by grep; the one hit in `LatteDecompilerEmitGLSLHeader.hpp:553` is a *comment*).
- Remove `dbus` (already `"platform": "linux"`, but delete for clarity).
- Keep `tiff` only if wxWidgets actually needs it — check with `vcpkg depend-info wxwidgets`; it's pulled in as a wx image handler dependency, so likely keep.
- Keep the `sdl3` 3.4.10 and `wxwidgets` 3.3.3 pins.

Also delete `dependencies/vcpkg_overlay_ports_linux/`, `dependencies/vcpkg_overlay_ports_win/`, and simplify `CMakeLists.txt:41-47` to a single unconditional `set(VCPKG_OVERLAY_PORTS "…/vcpkg_overlay_ports_mac;…/vcpkg_overlay_ports")`.

**Verify:** `vcpkg_installed/arm64-osx/` no longer contains `glslang`/`SPIRV-Tools`. Build time drops ~4 min on a cold cache.

### 1.3 Delete the x86-64 recompiler backend and all x86 branches (L)

**Delete:**
- `src/Cafe/HW/Espresso/Recompiler/BackendX64/` (all 9 files incl. `x86Emitter.h`, ~8500 lines) and its entry in `src/Cafe/CMakeLists.txt:86-94`. This is the single largest dead-code win — it currently compiles on arm64.
- `src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp:236-250` → keep only the `__aarch64__` branch; same at `:294-330` (register pools), `:614-680` (`PPCRecompiler_initPlatform` becomes empty — delete the function and its call at `:693`), `:684-690`.
- `src/Cafe/HW/Espresso/Recompiler/IML/IMLOptimizer.cpp:706-709` — delete the `ARCH_X86_64` eflags pass call.
- `src/Cafe/HW/Espresso/Recompiler/IML/IMLRegisterAllocator.cpp:126-…` — delete the x86 `GetInstructionFixedRegisters` overload; the `!g_CPUFeatures.x86.bmi2` check at `:161` goes with it.
- **`PPCRecompilerInstanceData_t` (`PPCRecompiler.h:130-151`)** — delete all 15 `_x64XMM_*` fields and `_x64XMM_mxCsr_*`. The struct then contains only `ppcRecompilerDirectJumpTable[0x4000000]` = exactly 512 MB. **Careful:** `PPCRecompiler.cpp:678` does `MemMapper::AllocateMemory(&ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom, sizeof(…) - offsetof(…), …)` — that call must be deleted, not just retargeted, or you'll commit a zero-size region. Replace with nothing; the jump table is committed lazily by `PPCRecompiler_reserveLookupTableBlock`.
- `src/Cafe/HW/Espresso/Debugger/GDBBreakpoints.{h,cpp}` — the entire hardware-breakpoint path is `ARCH_X86_64 && (LINUX|WINDOWS)`. On macOS arm64 it's already all `#else` stubs. **Recommendation: delete hardware breakpoints entirely** and leave GDBStub with software breakpoints only. Reimplementing them needs `thread_set_state`/`ARM_DEBUG_STATE64` and is not worth it now. Flag in the GDBStub log.
- `src/util/crypto/aes128.cpp:603-…` (AES-NI impl) and `:840-856` (the dispatch). **But do not just fall back to the software path** — see 1.4.
- `src/Cafe/HW/Latte/Core/LatteIndices.cpp` — remove the `ARCH_X86_64` branches at `:7, :347, :801, :816`; the `__aarch64__` NEON paths at `:9, :505, :808, :821` become unconditional.
- `src/Cafe/HW/Latte/Core/LatteTextureCache.cpp:150` (`g_CPUFeatures.x86.avx2`) — pick the non-AVX2 branch unconditionally or write a NEON version.
- `src/gui/wxgui/CemuUpdateWindow.cpp:122-128` — the platform string becomes a constant `"macos_bundle_aarch64"`. (The whole updater is deleted in §7.6 anyway.)

**Verify:** `grep -rn "ARCH_X86_64\|__x86_64__\|_M_X64\|x86Emitter\|IMLArchX86" src/` returns only hits inside `src/Common/unix/fast_float.h` (vendored third-party — leave it) and `src/util/crypto/md5.cpp:80` (endianness check, harmless — but simplify it to a `__LITTLE_ENDIAN__` check while you're there).

### 1.4 Rewrite `cpu_features` for arm64 (M)

`src/Common/cpu_features.cpp` / `.h`. The current file has a real bug (`#if BOOST_OS_MACOS` at `:37` shadows `#elif defined(ARCH_X86_64)` at `:54`, so macOS x86 builds never read CPUID) — moot after 1.3, but the file needs a full restructure.

New `CPUFeaturesImpl`:
```
struct { bool lse, lse2, aes, sha256, sha512, crc32, dotprod, fp16, i8mm, bf16, sme, sme2; } arm;
uint32 pageSize;         // sysctlbyname("hw.pagesize")
uint32 pCoreCount, eCoreCount;   // hw.perflevel0.physicalcpu, hw.perflevel1.physicalcpu
uint64 l1dCacheLineSize; // hw.cachelinesize
char   m_cpuBrandName[0x40];     // machdep.cpu.brand_string (keep, it works)
```
Read via `sysctlbyname` on: `hw.optional.arm.FEAT_LSE`, `FEAT_LSE2`, `FEAT_AES`, `FEAT_SHA256`, `FEAT_SHA512`, `FEAT_CRC32`, `FEAT_DotProd`, `FEAT_FP16`, `FEAT_I8MM`, `FEAT_BF16`, `FEAT_SME`, `FEAT_SME2`. Each returns an `int32` 0/1. Use a small `bool ReadSysctlBool(const char*)` helper.

Then rewrite `GetCommaSeparatedExtensionList()` to emit those, so `CafeSystem.cpp:625` logs something meaningful.

**Important caveat, be opinionated about it:** on Apple silicon these sysctls are *informational only*. Because clang's target baseline already guarantees `+lse +aes +sha2 +dotprod +fullfp16` (verified above), you must **not** gate compiled code paths on them — the compiler has already emitted LSE atomics unconditionally. Use them for the log line and for one thing only: **selecting the AES implementation.**

**Concrete win from this:** replace the deleted AES-NI path in `src/util/crypto/aes128.cpp` with an **ARMv8 crypto-extension implementation** using `<arm_neon.h>` `vaeseq_u8`/`vaesmcq_u8`/`vaesdq_u8`/`vaesimcq_u8`. Title/content decryption currently runs on the byte-wise `__soft__` path on every macOS build. This is a 5–10× speedup on WUA/WUD mount and NUS install. Since `__ARM_FEATURE_AES` is 1 unconditionally, you can drop the runtime dispatch entirely and delete the software path — or keep it behind `g_CPUFeatures.arm.aes` for paranoia. **Recommendation: keep the runtime check, it costs nothing.** *(This is arguably renderer-adjacent scope creep; if the other agent isn't covering crypto, it belongs here.)*

**Verify:** log line at boot reads e.g. `Used CPU extensions: LSE, LSE2, AES, SHA256, SHA512, CRC32, DotProd, FP16, I8MM, BF16` and `Apple M2`. Time a `.wua` mount before/after.

### 1.5 Normalize `__arm64__` → `ARCH_AARCH64` (S)

`src/Common/precompiled.h:29-31` currently defines `ARCH_X86_64` and has no aarch64 counterpart. Replace with:
```c
#if !defined(__aarch64__) || !defined(__APPLE__)
#error "This fork targets arm64 macOS only"
#endif
#define ARCH_AARCH64 1
```
Then fix the **Apple-only spelling** sites, which are latent portability bugs and confusing:
- `src/util/Fiber/FiberUnix.cpp:18` — `#ifdef __arm64__` guarding the `makecontext` 64-bit pointer split. (Deleted entirely in 1.6.)
- `src/Cafe/OS/libs/coreinit/coreinit_Thread.cpp:18, 32, 55, 1341` — the `arm_acle.h` include, the FPCR FTZ set (`enableFlushDenormalsToZero`), and the `__OSFiberThreadEntry(uint32,uint32)` signature split.

Replace all with `ARCH_AARCH64`, and since it's now unconditional, delete the `#if/#else` scaffolding outright. `enableFlushDenormalsToZero()` becomes a two-line function using `__arm_wsr64("fpcr", __arm_rsr64("fpcr") | (1u<<24))`.

Keep the arm64 intrinsic shims at `precompiled.h:356-384` but **fix two of them**:
- `_mm_mfence()` currently does `asm volatile("" ::: "memory")` *then* a `seq_cst` fence. The empty asm is redundant. Reduce to just `std::atomic_thread_fence(std::memory_order_seq_cst)`. Better: rename these to `cemu_cpu_pause()` / `cemu_cpu_timestamp()` / `cemu_full_barrier()` and delete the x86 names — masquerading as `_mm_*` on arm64 is a maintenance trap.
- `__rdtsc()` reads `cntvct_el0` (24 MHz virtual counter). Keep, but see 1.7.

**Verify:** `grep -rn "__arm64__" src/` returns nothing; `grep -rn "_mm_pause\|__rdtsc\|_mm_mfence" src/` returns only the shim definitions plus renamed call sites.

### 1.6 Rewrite the fiber implementation; then `_XOPEN_SOURCE` can go (M)

`src/util/Fiber/FiberUnix.cpp` is the guest-thread-switch hot path and has four problems:

1. **`swapcontext` saves and restores the signal mask.** On Darwin that's a `sigprocmask` syscall on *every* guest thread switch. Wii U games switch OSThreads thousands of times per second. This is pure overhead.
2. 2 MB `malloc`'d stacks with **no guard page** — a guest stack overflow silently corrupts the heap.
3. `ctx->uc_link = &ctx[0]` is self-referential (should be `nullptr`).
4. `_XOPEN_SOURCE` is force-defined process-wide at `src/CMakeLists.txt:17-25` *purely* to un-deprecate `<ucontext.h>`. On Darwin, `_XOPEN_SOURCE` without `_DARWIN_C_SOURCE` hides many BSD extensions from system headers — it happens to work today but it's a landmine every time you add a syscall.

**Recommendation: write a first-party AArch64 fiber.** It's ~50 lines of `.s`:
- Save/restore callee-saved GPRs `x19–x28`, `x29` (FP), `x30` (LR), `sp`, and the low 64 bits of `d8–d15`. That's 12×8 + 8×8 = 160 bytes — same layout the AArch64 backend already uses in `AArch64GenContext_t::enterRecompilerCode()` (`BackendAArch64.cpp:1628-1650`), so you can crib the register list.
- Allocate stacks with `mmap(PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON|MAP_STACK)` + one `PROT_NONE` guard page at the low end. **With 16 KB pages, round the stack size to a multiple of 16384** (2 MB already is; the guard page is 16 KB not 4 KB).
- Keep the `Fiber`/`Fiber::Switch` API identical so `coreinit_Thread.cpp` doesn't change beyond dropping the `__OSFiberThreadEntry(uint32,uint32)` split (a single `void*` arg is fine now).
- Drop both `std::atomic_thread_fence(seq_cst)` around the switch — the asm switch is a compiler barrier via the clobber list, and the `sCurrentFiber` TLS write is already ordered by it. Keep one `atomic_signal_fence` if you're nervous.

Then delete `_XOPEN_SOURCE` from `src/CMakeLists.txt:17-25` entirely. If you'd rather not write asm yet, the interim fix is `set_source_files_properties(util/Fiber/FiberUnix.cpp PROPERTIES COMPILE_DEFINITIONS _XOPEN_SOURCE)` so it stops leaking globally — do that in Phase 1 regardless, so the global define dies immediately.

**Verify:** boot a title; run a 60 s trace with `xctrace record --template 'Time Profiler'` and confirm `__sigprocmask` no longer appears under `Fiber::Switch`. Sanity-test the guard page by deliberately recursing in a guest function → expect SIGSEGV at the guard, not heap corruption.

**Flag as *not* worth doing:** don't try to move guest threads off fibers onto real threads. The scheduler in `coreinit_Thread.cpp` is deeply fiber-coupled and this is a multi-month rewrite.

### 1.7 PPC timebase: stop the 3-second startup stall (S)

`src/Cafe/HW/Espresso/PPCTimer.cpp:33-68` — `PPCTimer_estimateRDTSCFrequency()` busy-waits **3 seconds** measuring `cntvct_el0` against `HighResolutionTimer`. On Apple silicon the counter frequency is exactly known: `mach_timebase_info` gives numer/denom, and `cntfrq_el0` (readable from EL0 via `mrs`) gives 24000000 directly. Replace the whole function with a `mrs %0, cntfrq_el0` read, and delete the `invariant_tsc` check at `:36` along with the field.

**Why:** saves 3 s on every launch and removes a measurement that's less accurate than the hardware answer.

**Verify:** log the derived frequency; expect exactly `24000000`. Compare emulated-vs-wall clock over 60 s of gameplay.

### 1.8 Deployment target and platform-code purge (S)

- `CMakeLists.txt:102` → `set(CMAKE_OSX_DEPLOYMENT_TARGET "26.0")`
- `CMakeLists.txt` (after `project()`) → add:
  ```cmake
  if(NOT APPLE)
    message(FATAL_ERROR "This fork builds for arm64 macOS only")
  endif()
  set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "" FORCE)
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    message(FATAL_ERROR "Requires an Apple silicon host")
  endif()
  ```
  and delete the `CEMU_ARCHITECTURE` derivation at `:257-264` (make `xbyak_aarch64` unconditional) and the arch gate at `src/Cafe/CMakeLists.txt:621-627`.
- `src/CMakeLists.txt:113` → `MACOSX_MINIMUM_SYSTEM_VERSION "26.0"`; `src/resource/MacOSXBundleInfo.plist.in` `LSMinimumSystemVersion` is templated from it, so no literal edit needed — but §6.5 rewrites that plist anyway.
- `CMakeLists.txt:4` → `option(MACOS_BUNDLE "…" ON)`. **A non-bundle build cannot be signed, cannot carry entitlements, and therefore cannot use `MAP_JIT` under hardened runtime.** Making bundle the default is required, not cosmetic.
- Delete the entire `if(UNIX AND NOT APPLE)` and `if(WIN32)`/`if(MSVC)` machinery from `CMakeLists.txt` (Wayland, X11, GTK3, bluez, Feral GameMode, XInput, DirectInput, DirectSound, XAudio) and from `src/CMakeLists.txt:12-42, 230-241`.
- Delete `cmake/Findbluez.cmake`, `FindGTK3.cmake`, `FindWayland*.cmake`, `ECMFindModuleHelpers*.cmake`, `dependencies/gamemode/`, `dist/linux/`, `dist/windows/`, `src/resource/installer.nsi`, `src/resource/cemu.rc`, `src/resource/embedded/fontawesome.S` (keep `fontawesome_macos.S`), `src/audio/DirectSoundAPI.*`, `src/audio/XAudio*`, `src/input/api/XInput/`, `src/input/api/DirectInput/`.
- `src/audio/IAudioAPI` — `InitWFX()` is Windows-only with a "move this to Windows-specific" TODO; delete it. **Also convert `cubeb_devid`/device-ID plumbing from `std::wstring` to `std::string`** — it's a Windows-ism carried through `src/audio/IAudioAPI.h`, `CubebAPI.*`, and `src/config/CemuConfig.h`. (M, touches config serialization — needs a migration for existing `settings.xml`.)
- `.github/workflows/build.yml` — delete `build-ubuntu`, `build-appimage`, `build-windows` jobs and the `matrix: arch: [x86_64, arm64]` (§7).

**Verify:** `grep -rn "BOOST_OS_WINDOWS\|BOOST_OS_LINUX\|HAS_WAYLAND\|ENABLE_FERAL" src/ | wc -l` drops by >90%; `cmake --build build` clean.

### 1.9 CMake hygiene and codegen flags (S)

**Bugs to fix:**
- `src/CMakeLists.txt:37` — `if(CMAKE_C_COMPILER_ID STREQUAL "Clang")` never matches AppleClang (which reports `"AppleClang"`), *and* it tests the **C** compiler id for a **C++** warning. Fix: `if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")`. Once fixed, `-Wno-ambiguous-reversed-operator` will actually apply — **expect the build to get quieter, not to break**, but verify.
- `CMakeLists.txt:76-77` — `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON` with no `check_ipo_supported()` guard. Add:
  ```cmake
  include(CheckIPOSupported)
  check_ipo_supported(RESULT CEMU_IPO_OK OUTPUT _ipo_msg)
  option(CEMU_LTO "Enable ThinLTO for optimized builds" ${CEMU_IPO_OK})
  ```
  and drive `CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE/RELWITHDEBINFO` from `CEMU_LTO`.

**LTO — be opinionated:** keep **ThinLTO** (what CMake's IPO gives you with AppleClang), and **do not consider monolithic LTO**. With 8 GB unified memory, full LTO on this codebase will either thrash or OOM the linker. Additionally add a ThinLTO cache so relinks aren't 5 minutes:
```cmake
target_link_options(CemuBin PRIVATE
  "$<$<CONFIG:Release,RelWithDebInfo>:-Wl,-cache_path_lto,${CMAKE_BINARY_DIR}/ltocache>")
```
And **turn LTO off for `RelWithDebInfo`** (`CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO OFF`) — that's your daily-driver config and LTO makes iteration miserable. Keep it on for `Release` only.

**Flags to add:**
```cmake
add_compile_options(-fvisibility=hidden -fvisibility-inlines-hidden)
target_link_options(CemuBin PRIVATE -Wl,-dead_strip)
```
Small but free (smaller symbol table, better LTO internalization, ~2–4 % binary size). Watch out: `PPCRecompiler_getJumpTableBase` at `PPCRecompiler.cpp:566` is `extern "C" DLLEXPORT` (DLLEXPORT expands to nothing on non-Windows) — it exists for Cemuhook, which doesn't exist on macOS. Let it become hidden; if you want to keep it, annotate it `__attribute__((visibility("default")))`.

**Flags NOT to add — explicitly rejected:**
- **`-mcpu=apple-m1` / `apple-a14` / `-march=armv8.5-a`** — verified no-op; clang already defaults to `-target-cpu apple-m1` for `arm64-apple-macos`. Adding it only creates the false impression that you've tuned something, and pins you if Apple later moves the default up. **Don't.**
- **`-fno-semantic-interposition`** — an ELF/`-fPIC`-shared-object flag. Mach-O executables have no lazy symbol interposition to defeat. Pure noise. **Don't.**
- **`-mtune=apple-m2`** — would pessimize M3/M4 scheduling for a sub-1 % gain on M2. **Don't.**
- **BOLT** — `llvm-bolt` supports ELF only; there is no Mach-O backend. **Not an option on macOS. Say so in the docs so nobody re-litigates it.**
- **PGO** — technically available (`-fprofile-generate` / `llvm-profdata merge` / `-fprofile-use`). But in a recompiling emulator the hot instruction stream is JIT-generated machine code that PGO never sees; the AOT-compiled hot spots are the Latte command processor, texture/shader cache lookups, and the interpreter fallback. Expect 2–5 %. **Recommendation: defer to a later phase, gate behind `-DCEMU_PGO_USE=<profdata>`, measure, and keep only if >3 % on a real title.** Do not build it into the default pipeline — a stale profile is worse than none.

**PCH:** `src/CMakeLists.txt:47-54` recompiles `precompiled.h` per target on non-MSVC (~10 targets). Two options: (a) leave it — ninja parallelizes it and it's ~10 s total; (b) merge the ten libraries into fewer targets. **Recommendation: leave it.** The real win would be shrinking `precompiled.h` itself (it pulls `<future>`, `<sstream>`, `<iostream>`, all of boost) — that's a cross-cutting cleanup, not a Foundation blocker. Note it and move on.

**Verify:** `cmake --build build --target CemuBin` from clean; record wall time as your baseline. Confirm `-flto=thin` and `-cache_path_lto` in `build/compile_commands.json` / link line.

### 1.10 Remove the stale macOS disclaimer (S)
`src/gui/wxgui/CemuApp.cpp:388-402` — the "purely experimental… MoltenVk and Rosetta for ARM Macs" dialog is now factually wrong on every count. Delete it and the `did_show_macos_disclaimer` config key.

---

# Phase 2 — 16 KB page correctness

This is the phase most likely to be silently broken today.

### 2.1 Fix `MemMapperUnix.cpp` properly (S, high value)

`src/util/MemMapper/MemMapperUnix.cpp`. Three defects:

**(a) `AllocateMemory` rounds the base down but never extends the size.** `mprotect(page, size, …)` covers `page … page + roundup(size)`. When `baseAddr` is `off` bytes into a page, the region actually needed is `page … baseAddr + size = page + off + size`. If `off + size > roundup(size)`, the tail is **left unprotected**. Today the two callers that pass unaligned bases are `PPCRecompiler.cpp:678` (deleted in 1.3) and nothing else — but the bug is one refactor away from biting.

**(b) `FreeMemory` does no alignment fix-up at all and ignores the return value.** This is the live bug. `mmuRange_HIGHMEM` is `{0xFFFFF000, 0x1000}`. `memory_base` comes from `mmap(nullptr, 0x100000000, …)` and is 16 KB-aligned. `0xFFFFF000 % 0x4000 == 0x3000` → `mprotect(memory_base + 0xFFFFF000, 0x1000, PROT_NONE)` returns **`EINVAL`**, discarded. The range stays `PROT_READ|PROT_WRITE` across title unload/reload. `MMURange::mapMem()` then asserts `!m_isMapped` on the next `mapMem()` — that part is fine — but the *unmap* is a no-op, meaning stale guest memory survives a title switch. (Project Zero's out-of-bounds write at `0xfffffffe`, which this range exists to absorb, will read stale data from the previous session.)

**Fix:**
```c
static inline void AlignRange(void*& base, size_t& size)
{
    uintptr_t start = (uintptr_t)base;
    uintptr_t end   = start + size;
    uintptr_t aStart = start & ~(uintptr_t)(sPageSize - 1);
    uintptr_t aEnd   = (end + sPageSize - 1) & ~(uintptr_t)(sPageSize - 1);
    base = (void*)aStart;
    size = (size_t)(aEnd - aStart);
}
```
Apply in **both** `AllocateMemory(fromReservation=true)` and `FreeMemory(fromReservation=true)`. Check the `mprotect` return value in `FreeMemory` and `cemuLog_log(LogType::Force, …)` + `cemu_assert_debug` on failure. `AllocateMemory` must still return the **original unaligned** `baseAddr` on success, not the aligned page — callers depend on that.

**(c) `GetProt` `cemu_assert_unimplemented()` on `P_NONE`.** `FreeMemory` wants `PROT_NONE`; it hardcodes it, so this doesn't bite, but add a `P_NONE → PROT_NONE` case and delete the assert-only fallthrough.

**Also:** `AllocateMemory` reads `sysconf(_SC_PAGESIZE)` per call instead of the cached `sPageSize` at `:9-12`. Use the cached value.

**Verify:** add a debug-only self-test at startup that maps/unmaps every `MMURange` twice and asserts `mprotect` succeeded. Then: boot title A → return to game list → boot title B, and confirm no stale-memory assertion. Under `lldb`, `memory region 0xFFFFF000+memory_base` after unmap should read `---`.

### 2.2 Fix the 4 KB assumption in `MMU.h:77` (S)

`cemu_assert_debug((endAddress & 0xFFF) == 0)` in `MMURange::setEnd()`. This is the graphic-pack RAM-remap path (`MMU.cpp:190`, fed by `GraphicPack2::GetActiveRAMMappings()`).

Change to `MemMapper::GetPageSize() - 1`, i.e. `cemu_assert_debug((endAddress & (MemMapper::GetPageSize()-1)) == 0)`. **But** that will now fire on existing graphic packs that specify 4 KB-granular RAM extensions — which is exactly what you want to catch, except it turns a working pack into an assert.

**Recommendation:** don't assert. **Round `endAddress` *up* to the page size** inside `setEnd()`, and `cemuLog_log` when rounding occurred, naming the graphic pack. Rounding up is always safe here (the range grows into what was reserved-but-unmapped VA, and `MMU.cpp` already validates non-overlap against the next range afterwards — verify that the overlap check at `MMU.cpp:~195` runs *after* `setEnd`, and if not, reorder it).

### 2.3 Audit the MMU range table for 16 KB viability (S)

I computed every entry. Results:

| Range | Base | Size | 16 KB-aligned base? | 16 KB-multiple size? |
|---|---|---|---|---|
| `LOW0` | `0x00010000` | `0x000F0000` | yes | yes |
| `TRAMPOLINE_AREA` | `0x00E00000` | `0x00200000` | yes | yes |
| `CODECAVE` | `0x01800000` | `0x00400000` | yes | yes |
| `TEXT_AREA` | `0x02000000` | `0x0C000000` | yes | yes |
| `CEMU_AREA` | `0x0E000000` | `0x02000000` | yes | yes |
| `MEM2` | `0x10000000` | `0x40000000` | yes | yes |
| `OVERLAY_AREA` | `0xA0000000` | `0x1C000000` | yes | yes |
| `FGBUCKET` | `0xE0000000` | `0x04000000` | yes | yes |
| `TILINGAPERTURE` | `0xE8000000` | `0x02000000` | yes | yes |
| `MEM1` | `0xF4000000` | `0x02000000` | yes | yes |
| `RPLLOADER` | `0xF6000000` | `0x02000000` | yes | yes |
| `SHARED_AREA` | `0xF8000000` | `0x02000000` | yes | yes |
| `CORE0_LC` | `0xFFC00000` | `0x00005000` | **yes** | **no** (rounds to `0x8000`) |
| `CORE1_LC` | `0xFFC40000` | `0x00005000` | **yes** | **no** |
| `CORE2_LC` | `0xFFC80000` | `0x00005000` | **yes** | **no** |
| `HIGHMEM` | `0xFFFFF000` | `0x00001000` | **NO (`+0x3000`)** | no |

**Conclusion:** only two shapes are problematic, and neither needs the table changed.
- The three `*_LC` ranges over-map from `0x5000` to `0x8000` — harmless: the next range is `0x40000` away, so the extra `0x3000` lands in unmapped reservation. Guest code that reads there gets zeros instead of SIGSEGV. Acceptable. **Optionally** bump `initSize` to `0x8000` so the behaviour is explicit rather than accidental, and note it in a comment.
- `HIGHMEM` is the only misaligned base. With 2.1's fix it maps `0xFFFFC000 … 0x100000000` (48 KB) and unmaps the same. The over-mapped `0xFFFFC000–0xFFFFF000` is otherwise unused address space. **Recommendation: change the declaration to `{0xFFFFC000, 0x00004000}`** in `MMU.cpp:126` and update the comment. That makes the mapping exact and self-documenting, and removes the last unaligned range in the table. Guest code that touches `0xFFFFC000–0xFFFFEFFF` now succeeds instead of faulting — a behaviour change, but strictly more permissive and matching what the rounding already does.

**On the 4 GB reservation vs. 8 GB physical:** reserving `0x100000000` of VA is free (`PROT_NONE`, no commit) and works fine — 47-bit user VA on arm64 macOS. The real pressure is **commit**:
- `MEM2` = 1 GB committed on every title load
- `TEXT_AREA` = 224 MB, `OVERLAY` = 448 MB (optional), `FGBUCKET` = 64 MB, `MEM1`+`RPLLOADER`+`SHARED`+`CEMU` = 128 MB
- the recompiler jump table commits **8 MB of host memory per 4 MB of PPC code area touched** (`PPCRecompiler.cpp:496-525`, `PPC_REC_ALLOC_BLOCK_SIZE = 4 MB`, 8 bytes per PPC instruction). A game using all 224 MB of `TEXT_AREA` costs **448 MB** of jump table alone.
- `LatteBufferCache_init(164 MB)` at `LatteThread.cpp:120`
- Metal textures/buffers in the same unified memory pool

Realistic floor: **~2 GB RSS before any Metal allocations**, on an 8 GB machine also running the OS and the wx UI. **Action item for the risk register, and one cheap mitigation:** the pages are `MAP_ANON` and lazily faulted, so untouched `MEM2` costs nothing — but the jump-table `memset` loop at `PPCRecompiler.cpp:521-524` **touches every page it reserves**, forcing full commit of 8 MB per block. Consider initializing lazily (leave zeros, treat `nullptr` as "unvisited" in the dispatcher) — that's an M-sized change to the recompiler dispatcher and belongs to whoever owns the recompiler, but flag it from here.

**Verify:** `footprint -a $(pgrep Cemu)` and `vmmap -summary` during gameplay; compare "Dirty" against the budget above. Add the numbers to a boot-time log line.

---

# Phase 3 — JIT memory / W^X ownership

### 3.1 Decision: write a first-party `MAP_JIT` code allocator (L)

**Recommendation: yes, own it. Keep `xbyak_aarch64` as an instruction *encoder* only, and take over all memory management.**

Reasoning:

1. **Apple's one-region constraint is decisive.** With hardened runtime + `com.apple.security.cs.allow-jit`, the process may create **exactly one** `MAP_JIT` region. `xbyak_aarch64`'s `MmapAllocator` allocates per-`CodeGenerator`. `PPCRecompiler_generateAArch64Code` (`BackendAArch64.cpp:1438-1620`) constructs a fresh `AArch64Allocator` + `AArch64GenContext_t` **per recompiled function**. That's one `mmap` per function, potentially thousands. Under hardened runtime that fails after the first. You cannot ship this as-is.
2. **The cross-thread `pthread_jit_write_protect_np` hazard is real.** `pthread_jit_write_protect_np` is **per-thread** state, not per-region. Trampolines are generated on the init thread (`PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions`, called from `PPCRecompiler_init` at `PPCRecompiler.cpp:686`), while functions are generated on the worker thread (`PPCRecompiler_thread`, `:455`, → `:1612` region of `BackendAArch64.cpp`). If a third thread (a guest core executing recompiled code) happens to be inside a "write-enabled" window, it faults on execute. Today this is masked because the two producers never overlap in time — but it is fragile and undocumented.
3. **Recompiled code is never freed.** `PPCRecompiler_cleanupAArch64Code` (`BackendAArch64.cpp:1622-1628`) is defined and **never called**. `PPCRecompiler_deleteFunction` (`PPCRecompiler.cpp:573-589`) ends with `// todo - free x86 code`. Every `PPCRecompiler_invalidateRange` (self-modifying code, RPL unload, graphic-pack patching) leaks the generated code permanently. On an 8 GB machine with a long play session and a game that patches code, this is an unbounded leak.
4. `AArch64Allocator::setFreeDisabled(true)` at `BackendAArch64.cpp:1618` is exactly the hack you'd expect from bolting a general-purpose allocator onto a JIT — it disables `free` so the `CodeGenerator` destructor doesn't reclaim live code, which is why (3) happens.

### 3.2 Design: `JitCodeArena`

New files: `src/Cafe/HW/Espresso/Recompiler/JitCodeArena.{h,cpp}` (or `src/util/JitMemory/` if you want it renderer-agnostic — it isn't, keep it with the recompiler).

**Reservation.** One `MAP_JIT` region at init:
```c
void* base = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
```
`kArenaSize`: **256 MB**, configurable. Rationale: `BackendAArch64` already fails a function if any jump exceeds ±128 MB (`processAllJumps`, `BackendAArch64.cpp:1607-1611`), so keeping the whole arena within a 256 MB window means intra-arena branches are always in range, and you can use direct `b`/`bl` instead of the register-indirect path. It is reserved VA, not commit — actual dirty pages track generated code. `MAP_JIT` regions are lazily faulted like any anonymous mapping.

**Suballocation.** A simple bump allocator with a free list of size-classed blocks, aligned to **64 bytes** (`hw.cachelinesize`) — not to the page size, since W^X on Apple silicon is per-thread, not per-page. Round the arena itself to 16 KB (automatic; `mmap` size is page-granular).

**Write protocol.** Wrap every generation in an RAII guard:
```cpp
class JitWriteScope {           // per-thread, reentrant via a TLS depth counter
public:
    JitWriteScope()  { if (tDepth++ == 0) pthread_jit_write_protect_np(0); }
    ~JitWriteScope() { if (--tDepth == 0) pthread_jit_write_protect_np(1); }
private:
    static thread_local int tDepth;
};
```
Reentrancy matters because `readyRE()` and the jump-fixup pass both want write access.

**Publish protocol.** After the last write to a block and before any thread can branch to it:
```c
sys_icache_invalidate(blockStart, blockSize);   // <libkern/OSCacheControl.h>
std::atomic_thread_fence(std::memory_order_release);
// only now write the entrypoint into ppcRecompilerDirectJumpTable
```
Apple's doc is explicit that `sys_icache_invalidate` is mandatory on Apple silicon (I-cache is not coherent with D-cache). **There are currently zero occurrences of `sys_icache_invalidate`, `__builtin___clear_cache`, or `MAP_JIT` in `src/`** — all of it is inside `xbyak_aarch64`'s `CodeArray::protect`/`ready()`. Taking ownership means you must add it explicitly; forgetting it produces intermittent, unreproducible crashes on M-series (the worst possible failure mode).

**Cross-thread correctness.** The `JitWriteScope` is per-thread, so the worker thread being in write-mode does **not** affect guest-core threads executing from the same region — that is the actual semantic of `pthread_jit_write_protect_np` and it's what makes this safe. The one rule you must enforce: **no thread may branch into a block until the generating thread has left its `JitWriteScope` and called `sys_icache_invalidate`.** The existing `s_ppcRecompilerState.recompilerSpinlock` + the jump-table publish already give you the ordering; formalize it by making `JitCodeArena::Publish(block)` the *only* thing that writes `ppcRecompilerDirectJumpTable[…]`.

**Freeing.** Implement `JitCodeArena::Free(void* code, size_t size)` (coalescing free list), and **call it** from `PPCRecompiler_deleteFunction` (`PPCRecompiler.cpp:588`), replacing the `// todo - free x86 code` comment. Delete `PPCRecompiler_cleanupAArch64Code` and `AArch64Allocator::setFreeDisabled`. Freeing does not need a `JitWriteScope` (you're not writing), but do **not** reuse a freed block until you're sure no thread is executing it — the recompiler spinlock in `PPCRecompiler_invalidateRange` already serializes against `attemptEnter`, but a guest core could be *inside* a function whose range is being invalidated. **Recommendation: quarantine.** Freed blocks go to a quarantine list and are only returned to the free list at the next "all guest cores at a safepoint" boundary. `coreinit_Thread.cpp`'s scheduler lock (`__OSLockScheduler`) gives you such a boundary.

**Fallback for the interface trampolines.** `enterRecompilerCode_ctx` / `leaveRecompilerCode_*_ctx` (`BackendAArch64.cpp:1673-1697`) are three static `AArch64GenContext_t` globals with static-storage duration — they'd allocate from `xbyak`'s allocator at namespace scope. Convert them to allocate from the arena inside `PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions()`.

**`xbyak_aarch64` integration.** Subclass `Xbyak_aarch64::Allocator` with an implementation that calls into `JitCodeArena`, override `useProtect()` to return **`false`** (you own protection), and pass it to `AArch64GenContext_t`. This is exactly the seam `AArch64Allocator` already occupies — you're replacing its body, not its shape. Roughly 80 lines.

**Do NOT adopt `com.apple.security.cs.jit-write-allowlist`.** Apple's doc: adopting it means "your app can no longer call `pthread_jit_write_protect_np()`" and every write must go through a statically-allowlisted `pthread_jit_write_with_callback_np` callback that validates attacker-controlled input. That model fits a browser JS engine, not a recompiler that emits from a `CodeGenerator` object across hundreds of call sites. It would be a full inversion of the emitter's control flow for zero security benefit in a single-user emulator. Explicitly reject it and note why in a comment so it doesn't get re-proposed.

**Effort: L (1–2 weeks including the free-list and quarantine).**

**Verify:**
- `vmmap $(pgrep Cemu) | grep -i jit` shows exactly **one** region.
- Boot a title, then force `PPCRecompiler_invalidateRange` repeatedly (a game with self-modifying code, or a graphic pack with code patches) for 30 min; `footprint` shows the JIT region's dirty size plateauing instead of growing monotonically.
- Deliberately remove `sys_icache_invalidate` in a scratch build and confirm you can reproduce a crash — proves the call is load-bearing and not cargo cult.
- Run under `lldb` with `settings set target.process.stop-on-sharedlibrary-events` and single-step into freshly generated code.

---

# Phase 4 — P-core / E-core scheduling (4P + 4E)

**There is currently zero QoS anywhere** — no `pthread_set_qos_class_self_np`, no `QOS_CLASS_*`. On a 4P+4E M2 this is the largest untaken scheduling lever. But it is also the easiest way to make things *worse*, so the ordering below matters.

### 4.1 Fix the thread-count math first (S) — do this before any QoS

`GetPhysicalCoreCount()` (`src/util/helpers/helpers.cpp:235-271`) returns `std::thread::hardware_concurrency()` on non-Windows = **8** on M2. `MetalPipelineCache.cpp:54-60` then computes `numCompileThreads = 2 + (8 - 3) = 7`. Seven pipeline-compile threads, on a machine that simultaneously needs 3 guest-core threads + `LatteThread` on 4 P-cores. This is straightforward oversubscription and it is happening today.

Fix in `src/util/SystemInfo/SystemInfoMac.cpp` + `helpers.cpp`:
```c
uint32 GetPhysicalCoreCount();      // sysctl hw.physicalcpu        -> 8
uint32 GetPerformanceCoreCount();   // sysctl hw.perflevel0.physicalcpu -> 4
uint32 GetEfficiencyCoreCount();    // sysctl hw.perflevel1.physicalcpu -> 4
```
(Fall back to `hw.physicalcpu` if `hw.perflevel0.*` is absent — it won't be on Apple silicon, but a `sysctlbyname` miss should not be fatal.)

Then size **all** worker pools off `GetEfficiencyCoreCount()`, not the total: `MetalPipelineCache.cpp:54, 281`, and `RendererShaderMtl.cpp` compile threads. On M2 that gives 4 compile threads on 4 E-cores — correct.

**Leave `ActiveSettings.cpp:73` (`GetPhysicalCoreCount() >= 4` → MulticoreRecompiler) reading the *total* physical count.** Every Apple silicon Mac has ≥8, so it always picks multicore, which is what you want.

While you're in `SystemInfoMac.cpp`: `:28` and `:47` mix `out.size()` and `cpu_count` without a bounds check — `host_processor_info` returns `cpu_count` (8) but the loop at `:47` writes `out[i]` for `i < cpu_count` while `out` was sized by the caller. Add `const uint32 n = std::min<uint32>(cpu_count, out.size());`.

### 4.2 Central QoS assignment (S)

Extend the existing seam. `SetThreadName()` at `helpers.cpp:116-153` is already called at the top of every thread entry — add a companion, or better, replace it:

```cpp
enum class ThreadRole { GuestCore, GpuCommand, Recompiler, ShaderCompile,
                        Input, Background, Ui };
void ConfigureThread(const char* name, ThreadRole role);   // name + QoS in one call
```

Assignments for 4P+4E:

| Thread | Where | QoS | Rationale |
|---|---|---|---|
| wx main thread | `CemuApp::OnInit` | **leave alone** | AppKit already runs `main` at `USER_INTERACTIVE`. Overriding it is a common way to *lower* it by accident. |
| `OSSched[core=0..2]` (3× guest fiber hosts) | `coreinit_Thread.cpp:1395` | `QOS_CLASS_USER_INTERACTIVE` | Must never be demoted to an E-core. One demoted PPC core stalls the whole emulation, because the three cores are lock-step-ish via the scheduler. |
| `LatteThread` (GPU command processor) | `LatteThread.cpp:117` | `QOS_CLASS_USER_INTERACTIVE` | Frame-critical; feeds Metal. |
| `PPCRecompiler` worker | `PPCRecompiler.cpp:455` | `QOS_CLASS_UTILITY` | Off the critical path — the interpreter runs while it compiles. Putting it at UI priority steals the 4th P-core from `LatteThread`. **Conditional on 4.3.** |
| `mtlShaderComp`, `mtlAIRCache`, `compilePl`, `plCacheCompiler`, `plCacheWriter` | `RendererShaderMtl.cpp:81,111`; `MetalPipelineCache.cpp:27,585,599` | `QOS_CLASS_UTILITY` | Throughput work. Replaces the `; // TODO: set thread priority` no-op at `MetalPipelineCache.cpp:31`. |
| `SDL_events` / `Input_update` / `Wiimote-*` / `DSU-*` / `GCControllerAdapter::*` | `SDLControllerProvider.cpp:291`, `InputManager.cpp:933`, etc. | `QOS_CLASS_USER_INITIATED` | Latency-sensitive, near-zero CPU. Not INTERACTIVE — they don't need a P-core, they need to not be starved. |
| `iosu*`, `boss*`, `bootsnd`, `GDBServer`, file/shader cache loaders | various | `QOS_CLASS_UTILITY` | |
| cubeb / CoreAudio HAL IO thread | created by CoreAudio | **do not touch** | See 4.4. |

**That's 4 threads at `USER_INTERACTIVE` on exactly 4 P-cores.** This is the whole design: 3 guest cores + Latte saturate the P-cluster, and everything else is explicitly pushed to the E-cluster. On an M3 Pro/Max (more P-cores) it degrades gracefully; on a base M4 (4P+6E) it's identical.

**Note on the "4th P-core":** the wx main thread is also `USER_INTERACTIVE` and will contend. It's near-idle during gameplay (the 5 ms SDL pump timer is the main consumer — see 5.3), so this is acceptable. If profiling shows contention, the fix is 5.3 (move SDL off the main thread), not lowering the main thread's QoS.

### 4.3 QoS inversion — the one thing that will actually bite you (M)

Darwin gives you priority donation for free on `pthread_mutex_t` (and therefore `std::mutex`), `std::condition_variable`, and `os_unfair_lock`. It gives you **nothing** for pure spin loops and `std::atomic` polling.

**`src/util/helpers/fspinlock.h` is a pure spin loop** (`m_lockBool.exchange` + `_mm_pause()` — which on arm64 is the `yield` shim from `precompiled.h:358`). `s_ppcRecompilerState.recompilerSpinlock` is taken by:
- the `PPCRecompiler` worker (which you just put at `UTILITY` → E-core), and
- guest core threads at `USER_INTERACTIVE` via `PPCRecompiler_invalidateRange` (`PPCRecompiler.cpp:570`), `PPCRecompiler_deleteFunction`, and the enqueue path.

A `USER_INTERACTIVE` guest core spinning on a lock held by a descheduled `UTILITY` worker on a saturated E-cluster will spin for a full scheduler quantum. `yield` on arm64 does not donate priority. **This is a hard hang risk under load, not a slowdown.**

**Fix, in order of preference:**
1. **Replace `FSpinlock` with `os_unfair_lock`** for `recompilerSpinlock` specifically (and audit other `FSpinlock` users the same way). `os_unfair_lock` does priority donation on Darwin and is as cheap as a spinlock in the uncontended case. `FSpinlock` already implements `BasicLockable`, so a drop-in `class FSpinlock { os_unfair_lock m_l = OS_UNFAIR_LOCK_INIT; … }` keeps every call site. The `is_locked()` accessor used by `cemu_assert_debug` at `PPCRecompiler.cpp:575` needs `os_unfair_lock_assert_owner` instead — that's actually stronger.
2. Only if (1) is somehow infeasible: raise the recompiler worker to `QOS_CLASS_USER_INITIATED`. This wastes a P-core and is the inferior answer.

**Audit list — grep for these patterns and check each against the QoS table:**
- `FSpinlock` users: `grep -rn "FSpinlock" src/`
- `while (!flag)` / `std::this_thread::sleep_for` polling loops. **`Latte_Start()` at `LatteThread.cpp:226-231` polls `sLatteThreadFinishedInit` with 1 ms sleeps from the main thread** — harmless (bounded, startup-only) but replace with a `std::binary_semaphore` while you're there.
- `std::atomic` + `yield` spin in `src/util/helpers/Semaphore.h`, `ConcurrentQueue.h`.

**Recommendation: do 4.3 *before* 4.2.** Adding QoS to a codebase with unfixed spinlock inversions converts a latent bug into a reproducible hang.

### 4.4 Audio: do not set QoS, fix the callback instead (M)

`src/audio/CubebAPI.cpp:40` takes a `std::unique_lock` **inside `data_cb`**, which runs on the CoreAudio HAL IO thread. That thread runs under a Mach **time-constraint (real-time) thread policy**, strictly above every QoS class. When a `UTILITY`-class producer holds `m_mutex` and the RT thread blocks on it, you get a glitch — and `pthread_mutex` donation cannot help, because there is no QoS class high enough to represent "real-time."

**Fixes, in order:**
1. **Replace the `std::vector<uint8> m_buffer` + mutex with a lock-free SPSC ring buffer.** `data_cb` becomes wait-free: read the atomic head/tail, `memcpy`, publish. `FeedBlock`/`NeedAdditionalBlocks` become the producer side. This is the correct fix and it's ~100 lines.
2. Note that `data_cb` also calls `m_buffer.erase(begin, begin+copied)` — a `std::vector` erase-from-front is **O(n) with a memmove of up to 4× `m_bytesPerBlock` on the realtime thread**. The ring buffer kills this too.
3. **Latency policy.** `CubebAPI.cpp:91-92` always requests `cubeb_get_min_latency()`, unclamped. On CoreAudio that's typically 512 frames @ 48 kHz ≈ 10.7 ms — but it can come back much smaller, and the Wii U's `AI` block feeds audio at a coarse cadence tied to guest scheduling. Requesting a smaller buffer than the producer can reliably fill guarantees underruns. **Recommendation: `latency = clamp(cubeb_get_min_latency(), 480, 1920)` frames** (10–40 ms @48 kHz), exposed as a 3-way user setting (Low / Balanced / Safe → 480 / 960 / 1920). Default **Balanced (960 ≈ 20 ms)** — on an 8 GB M2 running an emulator, robustness beats 10 ms of latency, and 20 ms is inaudible for this use case.
4. Do **not** call `pthread_set_qos_class_self_np` from inside `data_cb`. It's already above every QoS class; setting one would *demote* it.

**Optional, later:** join the `LatteThread` to the audio device's `os_workgroup` (`kAudioDevicePropertyIOThreadOSWorkgroup`) so the scheduler treats the render and audio threads as one deadline group. `<os/workgroup.h>` is present in the SDK (verified). **Recommendation: skip for now** — it's a real win only once you have stable frame pacing, and cubeb doesn't expose the device's workgroup. Revisit after the Metal renderer stabilizes.

### 4.5 `ThreadPool` (S — or skip)

`src/util/ThreadPool/ThreadPool.h` is 15 lines of `std::thread(...).detach()`. Every `FireAndForget` spawns and leaks a thread.

**Recommendation: don't write a thread pool. Replace `ThreadPool::FireAndForget` with `dispatch_async` on a global concurrent queue at the right QoS:**
```cpp
static void FireAndForget(auto&& f, auto&&... args) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ ... });
}
```
libdispatch already has a correctly-sized, QoS-aware, P/E-aware thread pool tuned by the kernel. Writing your own on a 4P+4E machine is strictly worse. This also gets you automatic QoS override propagation for the Metal compile pools if you migrate them to dispatch queues (a `dispatch_block_wait` from `LatteThread` automatically boosts the compiling block to `LatteThread`'s QoS — solving the "synchronous pipeline compile blocks the render thread" case for free).

**Caveat:** the block must not capture C++ objects with non-trivial destructors across a `-fno-objc-arc` boundary carelessly; use `__block` or a heap-allocated `std::function` trampoline. Also `dispatch_async` from a guest fiber is fine (dispatch doesn't care about ucontext), but do not `dispatch_sync` from a fiber to a queue that might re-enter guest code.

**Verify Phase 4:** `xctrace record --template 'System Trace'` for 60 s of gameplay; in the CPU lane confirm the three `OSSched` threads and `LatteThread` sit on P-cores and `compilePl`/`PPCRecompiler` sit on E-cores. Watch for `Thread State` → `Blocked` on the audio thread (should be zero).

---

# Phase 5 — Native macOS integration

### 5.1 Info.plist (S)

`src/resource/MacOSXBundleInfo.plist.in` is missing everything modern. Add:

| Key | Value | Why |
|---|---|---|
| `NSHighResolutionCapable` | `true` | **Without this, macOS runs the app in 1× magnified mode.** On a Retina display everything is blurry and the Metal drawable is half resolution. This is the single highest-impact missing key. |
| `NSSupportsAutomaticGraphicsSwitching` | `true` | No-op on Apple silicon (single GPU) but harmless and correct. **Low priority.** |
| `NSPrefersDisplaySafeAreaCompatibilityMode` | `false` | Notched MacBook displays — lets you use the full height in fullscreen. |
| `NSHumanReadableCopyright` | already present | |
| `LSApplicationCategoryType` | `public.app-category.games` — already set via `MACOSX_BUNDLE_CATEGORY` | **This is the Game Mode trigger.** See 5.2. |
| `LSMinimumSystemVersion` | `26.0` (templated) | |
| `NSCameraUsageDescription` | — | not needed |
| `NSBluetoothAlwaysUsageDescription` | "Cemu uses Bluetooth to connect Wii Remotes." | **Required.** Wiimote support goes through SDL3's HID API; on macOS, Bluetooth device enumeration triggers TCC. Without the string the app is **killed** on first access, not merely denied. |
| `NSLocalNetworkUsageDescription` | "Cemu uses the local network for DSU motion servers and Wii U online emulation." | `DSUControllerProvider` binds UDP; macOS 15+ gates local-network access. Without it, DSU silently fails. |
| `NSAppleEventsUsageDescription` | — | only if you script Finder; you don't |
| USB | — | **No usage-description key exists for libusb/IOUSBHost in a non-sandboxed app.** The GameCube adapter works via libusb without TCC. Do *not* add a bogus key. |
| `CFBundleDocumentTypes` | extend beyond `.wua` | currently only `${MACOSX_BUNDLE_TYPE_EXTENSION}` = `wua`. Add `wud`, `wux`, `wuhb`, `rpx`, `iso`. Cheap UX win. |
| `UTExportedTypeDeclarations` | for `.wua` | proper UTI declaration; optional |

Also fix the plist's inconsistent tab/space indentation while you're in there (lines 29-32 are visibly mangled).

**Verify:** `plutil -lint` the generated `Contents/Info.plist`; launch and confirm the window is crisp on a Retina display (`CGDisplayCopyDisplayMode` backing scale 2.0 in the Metal drawable).

### 5.2 Game Mode (S)

**What's actually required on macOS 26:** Game Mode is activated **automatically by the system** — there is no entitlement, no opt-in Info.plist boolean, and no runtime API to request it. The gate is:
1. `LSApplicationCategoryType` is a game category (`public.app-category.games` or a subcategory) — **already set** at `src/CMakeLists.txt:110`, so this is done.
2. The app is in **fullscreen** on its display.
3. The system decides based on sustained GPU/CPU usage.

When active, macOS gives the app **priority access to CPU and GPU**, doubles the Bluetooth sampling rate for game controllers and AirPods (halving input and audio latency), and shows a controller glyph in the menu bar.

**So the work here is not plist work — it's making sure the app actually goes fullscreen.** Concretely:
- The wx main window must use real AppKit fullscreen (`NSWindowCollectionBehaviorFullScreenPrimary`), which wxWidgets does via `ShowFullScreen(true, wxFULLSCREEN_ALL)`. Verify `src/gui/wxgui/MainWindow.cpp`'s fullscreen path does this and not a borderless-window fake.
- Do not create a second borderless window over the display; Game Mode won't recognize it.

**I could not find a canonical Apple documentation page enumerating Game Mode's activation criteria** — it's described in WWDC session material rather than reference docs. **Verify empirically, do not assume:** with a title running fullscreen, check the menu-bar Game Mode indicator, and `log stream --predicate 'subsystem CONTAINS "gamepolicy"' --level debug` to see the daemon's decision. If it doesn't engage, the category or the fullscreen mode is wrong.

**Effort: S. Payoff: real (the Bluetooth polling-rate doubling directly helps controller latency, which is the thing 5.3 is also about).**

### 5.3 SDL event pump: the 5 ms `wxTimer` problem, and the `GameController.framework` question (M)

Current state: on macOS only, `CemuApp::OnInit` (`CemuApp.cpp:345-349`) starts a 5 ms `wxTimer` that calls `SDLControllerProvider::PumpSDLEvents()` on the main thread. Every other platform uses a dedicated SDL event thread (`SDLControllerProvider.cpp:19-26`, gated by `#if !BOOST_OS_MACOS`). Consequences:
- Controller polling is capped at **~200 Hz** — and in practice much worse, because `wxTimer` is coalesced with the run loop and will not fire during modal dialogs, menu tracking, or window resize.
- Input latency is coupled to whatever else the wx main loop is doing.
- It burns main-thread CPU 200×/s during gameplay, contending for the P-core budget from §4.2.

**Recommendation: stay on SDL3. Do not adopt `GameController.framework`.**

Reasoning — and I want to be clear this is a close call that I'm deliberately deciding:
- `GameController.framework` (`GCController`) is genuinely better on Apple platforms: no HID parsing, native support for DualSense/DualShock/Xbox/Switch Pro including haptics, battery, motion, and adaptive triggers, plus it's the path Game Mode's Bluetooth polling boost is tuned for. And it does not require a main-thread pump.
- **But** the input layer here is not small. `src/input/api/SDL/` is one of five `ControllerProvider` implementations, and SDL3 is *also* carrying the Wiimote HID transport (`WiimoteControllerProvider` uses SDL3's HID API, not libhidapi). Adopting `GameController` means writing a sixth provider, keeping SDL anyway for Wiimotes, and maintaining two mapping databases. That's weeks of work for a latency win you can get in an afternoon.
- The 200 Hz cap is not a *SDL* problem. It's a "we pump SDL from a wxTimer" problem.

**Do this instead (S, high value):** delete the macOS special case entirely and use the same dedicated SDL event thread as every other platform.
- `src/input/api/SDL/SDLControllerProvider.cpp:19-26` and `:29-55` — remove `#if !BOOST_OS_MACOS`.
- `src/input/api/SDL/SDLControllerProvider.h:27-40` — remove the `PumpSDLEvents`/macOS-only declarations.
- `src/gui/wxgui/CemuApp.cpp:341-350, 434-441` — delete `m_sdlEventPumpTimer` and `OnSDLEventPumpTimer` entirely.

**The catch that presumably caused the workaround in the first place:** SDL's *video* subsystem must be initialized and pumped on the main thread on macOS (it drives `NSApplication`). But the joystick/gamepad subsystem does not — `SDL_WaitEvent` on a background thread is supported for gamepad events as long as `SDL_INIT_VIDEO` was never initialized. Check `SDLControllerProvider::InitSDL()`: if it passes `SDL_INIT_VIDEO` or `SDL_INIT_EVENTS` together with video, that's the bug. Initialize **only** `SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC` and it works from a background thread.

This interacts with 5.4: `ScreenSaver::SetInhibit` (`src/util/ScreenSaver/ScreenSaver.h:34-40`) **does** `SDL_InitSubSystem(SDL_INIT_VIDEO)`, on the main thread, inside a wx app that already owns `NSApplication`. That is almost certainly the "feature crashes on macOS" from the config comment. Killing the SDL video path (5.4) is a prerequisite for the SDL thread migration.

Set the SDL thread to `QOS_CLASS_USER_INITIATED` per §4.2.

**Revisit `GameController.framework` later** if you want DualSense haptics/adaptive triggers or Game Mode's controller integration. Write it down as a deferred decision with this reasoning, so it isn't re-argued.

### 5.4 Screensaver inhibition: replace SDL with `NSProcessInfo` (S)

Delete the SDL implementation in `src/util/ScreenSaver/ScreenSaver.h` and the two "temporary workaround" blocks at `src/config/CemuConfig.h:412-419` and `src/gui/wxgui/GeneralSettings2.cpp:251-253, 1880-1882`. Restore `disable_screensaver` to default `true`.

New implementation, new file `src/util/ScreenSaver/ScreenSaverMac.mm`:
```objc
static id<NSObject> s_activity;   // NSProcessInfo activity token
void ScreenSaver::SetInhibit(bool inhibit) {
    if (inhibit && !s_activity) {
        s_activity = [[NSProcessInfo processInfo]
            beginActivityWithOptions:(NSActivityIdleDisplaySleepDisabled |
                                      NSActivityIdleSystemSleepDisabled |
                                      NSActivityUserInitiated)
                              reason:@"Emulating a game"];
        [s_activity retain];
    } else if (!inhibit && s_activity) {
        [[NSProcessInfo processInfo] endActivity:s_activity];
        [s_activity release];
        s_activity = nil;
    }
}
```

**Use `NSProcessInfo`, not `IOPMAssertionCreateWithName`.** Both work, but `NSProcessInfo -beginActivityWithOptions:` is the documented modern API, is automatically released if the process dies (an orphaned `IOPMAssertion` keeps the user's display awake until reboot — a genuinely bad failure mode), and additionally disables App Nap and timer coalescing via `NSActivityUserInitiated`, which is exactly what you want during emulation.

It's thread-safe to call from any thread, so the `SDL_RunOnMainThread` dance disappears. Callers are already correct: `MainWindow.cpp:593` (title start) and `:1772` (title stop), plus `GeneralSettings2.cpp:1145`.

`enable_language(OBJC OBJCXX)` is already active (`CMakeLists.txt:101`), so a `.mm` file drops straight in.

**Verify:** `pmset -g assertions` shows `PreventUserIdleDisplaySleep` while a title runs and nothing when stopped. Quit the app mid-game via `kill -9` and confirm the assertion is gone.

### 5.5 Replace Carbon key codes (S)

`src/gui/wxgui/wxWindowSystem.cpp:17-19` includes `<Carbon/Carbon.h>` for `kVK_Control`, `kVK_RightControl`, `kVK_Tab`, `kVK_Escape` at `:250-263`.

Carbon still exists in macOS 26 but is a 30-year-old deprecated umbrella that drags in HIToolbox and QuickDraw headers, and pulling it into a C++20 TU is a source of macro collisions (`Point`, `Rect`, `Boolean`, `nil`).

**Fix:** the `kVK_*` constants live in `<HIToolbox/Events.h>` but the supported modern home is **`<Carbon/HIToolbox/Events.h>`**, which is still the only public declaration. Rather than chase that, just **define the four constants locally** — they are ABI-frozen hardware key codes that have not changed since 1984:
```c
enum { kVKLocal_Control = 0x3B, kVKLocal_RightControl = 0x3E,
       kVKLocal_Tab = 0x30, kVKLocal_Escape = 0x35 };
```
with a comment pointing at `Carbon/HIToolbox/Events.h`. Delete the `<Carbon/Carbon.h>` include. This is a two-line change that removes an entire deprecated framework from the link line.

`WindowSystem::IsKeyDown(key)` presumably calls `CGEventSourceKeyState` — verify it does, and if it uses a Carbon API, switch to `CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, keycode)` from CoreGraphics.

**Verify:** `otool -L bin/Cemu_release.app/Contents/MacOS/Cemu | grep -i carbon` returns nothing. Hotkeys still work.

### 5.6 `_Exit()` teardown — keep it, but narrow it (S)

`CemuApp::OnExit` (`CemuApp.cpp:429-431`) calls `_Exit(retValue)` on non-Windows, skipping all static destructors.

**Recommendation: keep it.** This is not laziness, it's correct for this program:
- There are dozens of detached threads (`ThreadPool::FireAndForget`, `iosu*`, `boss`, shader compile pools) with no join protocol. A clean `exit()` runs static destructors while those threads are still touching the objects being destroyed → SIGSEGV at quit, every time. The Linux comment in `ExceptionHandler_posix.cpp:135-141` says exactly this.
- Guest threads are ucontext fibers on manually-managed stacks. Unwinding them at exit is not defined behavior.
- Fixing this properly means giving every subsystem a shutdown protocol — that's a multi-week refactor with zero user-visible benefit.

**But narrow it:** before `_Exit`, explicitly flush the things that *must* persist:
1. `GetConfigHandle().Save()` — verify this happens on every exit path, not just Settings-OK.
2. Flush and `fsync` the shader/pipeline caches and `FileCache`. This connects to the `#ifdef __APPLE__` double-`Flush()` at `src/Cemu/FileCache/FileCache.cpp:465-473` — that Flush-after-every-write exists precisely *because* `_Exit` can drop buffered writes. **Once you have an explicit flush-on-exit, delete the per-write `Flush()`** — it's a synchronous write amplification on every cache entry and it's the reason shader cache builds feel slow on macOS. (Note `Flush()` is a userspace flush, not `fsync`; for durability across a *crash* you'd need `F_FULLFSYNC`, but you only need durability across a clean `_Exit`, so a userspace flush at exit suffices.)
3. `cemuLog` flush.

Add a `Cemu_FlushPersistentState()` called immediately before `_Exit`.

**Verify:** quit during a shader-cache build, relaunch, confirm the cache is intact and no entries were lost. Benchmark shader cache build time before/after removing the per-write flush.

---

# Phase 6 — Signing, notarization, distribution

Current state is broken in five independent ways. Enumerating them, because each needs a different fix:

1. `codesign … bin/Cemu_app/Cemu.app/Contents/MacOS/Cemu` signs the **inner Mach-O**, not the `.app`. Gatekeeper evaluates the bundle. The bundle is therefore unsigned.
2. `--deep` is deprecated by Apple and does the wrong thing (it re-signs nested code with the *outer* entitlements). Nested `libMoltenVK.dylib` / `libusb-1.0.0.dylib` are in `Contents/Frameworks/` — with `--deep` on the inner binary path, they're **never signed at all**.
3. `--entitlements X --preserve-metadata=entitlements` is self-contradictory; codesign resolves in favor of `--preserve-metadata`, so **`--entitlements` is silently ignored**. The binary gets whatever entitlements it already had (none).
4. `--sign -` (ad-hoc) with `--timestamp`: **secure timestamps require a real identity**; ad-hoc + `--timestamp` is meaningless at best.
5. **The entitlements file is never referenced from CMake at all.** A local `MACOS_BUNDLE=ON` build is completely unsigned and un-entitled — which after Phase 3 means **`MAP_JIT` under hardened runtime will fail and the emulator won't run.** This is now a hard blocker, not a packaging nicety.

### 6.1 Entitlements: two files (S)

Replace `src/resource/cemu.macos.entitlements` with two files in a new `dist/macos/`:

**`dist/macos/Cemu.entitlements`** (release + local dev):
```xml
<dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
</dict>
```
That's it. Reasoning for each removal:

- **`com.apple.security.get-task-allow` → REMOVE.** Its presence causes **notarization to fail outright**. It must never appear in a distributed build. Keep it in a *separate* debug-only file (below).
- **`com.apple.security.cs.allow-unsigned-executable-memory` → REMOVE.** Reasoning it out: this entitlement exists to permit `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)` *without* `MAP_JIT`. On **arm64 macOS a plain anonymous RWX mapping always fails regardless of entitlements** — the hardware and kernel enforce that executable pages come from a `MAP_JIT` region or a signed mapping. So on arm64 this entitlement grants literally nothing. After Phase 3, all executable memory comes from one `MAP_JIT` region covered by `allow-jit`. It is dead weight that weakens the app's security posture and invites notarization scrutiny. **Delete it.** (If you delete it and JIT breaks, that means Phase 3 isn't actually using `MAP_JIT` — which is a Phase 3 bug, not a reason to restore the entitlement.)
- **`com.apple.security.cs.disable-library-validation` → REMOVE.** This exists to load dylibs not signed by the same Team ID. Its only justification was `libMoltenVK.dylib` copied from `/usr/local/lib` (signed by Khronos, or unsigned). **MoltenVK is deleted in Phase 1.** The only remaining nested dylib is `libusb-1.0.0.dylib`, which *you* build and *you* sign with your own identity — so library validation passes. Delete it. **Caveat:** if you ever want to load user-supplied plugins (Cemuhook-style), you'd need it back; there's no such thing on macOS, so don't pre-emptively keep it.

**`dist/macos/Cemu.debug.entitlements`** (local debugging only, never distributed):
```xml
<dict>
  <key>com.apple.security.cs.allow-jit</key><true/>
  <key>com.apple.security.get-task-allow</key><true/>
</dict>
```
`get-task-allow` is what lets `lldb` attach. Local builds need it; release builds must not have it.

### 6.2 Sign from CMake, always (M) — this is the actual blocker

Add to `src/CMakeLists.txt` under `if(MACOS_BUNDLE)`:

```cmake
set(CEMU_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity; '-' = ad-hoc")
set(CEMU_ENTITLEMENTS "${CMAKE_SOURCE_DIR}/dist/macos/Cemu.$<IF:$<CONFIG:Debug,RelWithDebInfo>,debug.,>entitlements")

# sign inner-out: nested dylibs first, bundle last
add_custom_command(TARGET CemuBin POST_BUILD
  COMMAND codesign --force --options runtime --timestamp=none
          --sign "${CEMU_CODESIGN_IDENTITY}"
          "${FRAMEWORKS_DIR}/libusb-1.0.0.dylib"
  COMMAND codesign --force --options runtime --timestamp=none
          --entitlements "${CEMU_ENTITLEMENTS}"
          --sign "${CEMU_CODESIGN_IDENTITY}"
          "$<TARGET_BUNDLE_DIR:CemuBin>"
  VERBATIM)
```

Rules encoded here:
- **Sign inner-out.** Every nested Mach-O first, then the bundle. Signing the bundle seals the nested signatures into `_CodeSignature/CodeResources`; if you sign a nested dylib afterwards you invalidate the outer signature.
- **Never `--deep`.**
- **Never combine `--entitlements` with `--preserve-metadata=entitlements`.** Drop `--preserve-metadata` entirely.
- `--options runtime` (hardened runtime) is required for notarization **and** is what makes `allow-jit` meaningful.
- `--timestamp=none` for local/ad-hoc builds (a secure timestamp needs a real identity and network); CI overrides to `--timestamp`.
- Sign the bundle **after** all the `POST_BUILD` copies (update.sh, gameProfiles, resources, libusb) — CMake runs `POST_BUILD` commands in declaration order, so this block must come last. Getting this wrong produces "resource envelope is obsolete" at launch.

**This makes local `MACOS_BUNDLE=ON` builds ad-hoc-signed with the JIT entitlement, which is exactly what a developer without a paid Developer ID needs.** Ad-hoc signature + `--options runtime` + `com.apple.security.cs.allow-jit` is sufficient for `MAP_JIT` to succeed. No Apple Developer Program membership required. Document this in `BUILD.md` — it's the #1 thing a new contributor will trip over.

**Verify:**
```
codesign -dv --entitlements - --verbose=4 bin/Cemu_release.app
codesign --verify --strict --verbose=2 bin/Cemu_release.app
spctl -a -vvv -t exec bin/Cemu_release.app     # expect "rejected" for ad-hoc; that's correct
```
And most importantly: launch it and confirm the JIT arena `mmap` succeeds.

### 6.3 Notarization + stapling in CI (M)

Replace the `Prepare artifact` step in `.github/workflows/build.yml`. Sequence:

1. Import the Developer ID Application certificate into a temporary keychain (`security create-keychain`, `security import`, `security set-key-partition-list`).
2. Build with `-DCEMU_CODESIGN_IDENTITY="Developer ID Application: <Name> (<TEAMID>)"` and `--timestamp` (network timestamp is **required** for notarization).
3. Verify: `codesign --verify --deep --strict --verbose=2 Cemu.app` (`--deep` is fine for *verification*, just not for signing).
4. **Notarize the DMG, not the app** — one submission, and stapling the DMG means the ticket travels with the download:
   ```
   hdiutil create -volname Cemu -srcfolder Cemu_app -ov -format UDZO Cemu.dmg
   codesign --force --sign "$IDENTITY" --timestamp Cemu.dmg
   xcrun notarytool submit Cemu.dmg --keychain-profile CemuNotary --wait
   xcrun stapler staple Cemu.dmg
   xcrun stapler validate Cemu.dmg
   ```
5. Store credentials once with `xcrun notarytool store-credentials` using an **App Store Connect API key** (`--key`, `--key-id`, `--issuer`), not an app-specific password — API keys don't expire and don't break when the account gets 2FA changes.
6. On failure: `xcrun notarytool log <submission-id>` gives per-binary reasons. The two you'll actually hit are `get-task-allow` present (6.1) and a nested binary not signed with hardened runtime (6.2 inner-out ordering).

**Also:** the CI currently does `sed -i '' 's/Cemu_release/Cemu/g' Contents/Info.plist` **after** the app is built. Any `Info.plist` mutation must happen **before** signing or it invalidates the signature. Better: fix it at the source — set `OUTPUT_NAME "Cemu"` for bundle builds instead of `Cemu_$<LOWER_CASE:$<CONFIG>>` (`src/CMakeLists.txt:97`), and delete the whole rename/sed dance.

**Runner:** `macos-14` in the current workflow is Apple silicon, good. Bump to `macos-15` or `macos-26` for an Xcode 26 SDK (required for a macOS 26 deployment target). Delete the `matrix: arch: [x86_64, arm64]`.

**Verify:** on a *different* Mac, download the DMG through a browser (so it gets the quarantine bit), open it, drag to /Applications, launch. No Gatekeeper prompt at all. `spctl -a -vvv -t exec /Applications/Cemu.app` → `accepted, source=Notarized Developer ID`.

### 6.4 The libusb dylib (S)

`dependencies/vcpkg_overlay_ports_mac/libusb/portfile.cmake` forces `VCPKG_LIBRARY_LINKAGE dynamic`, which is the sole reason for the `Contents/Frameworks/` directory, the `install_name_tool -change` at `src/CMakeLists.txt:180-186`, and the `INSTALL_RPATH "@executable_path/../Frameworks"`. Everything else in the `arm64-osx` triplet is static.

**Do not switch it to static.** libusb is **LGPL-2.1**; static linking obliges you to distribute relinkable object files. Dynamic linking is the licence-compliant choice and the reason the overlay port exists. Keep it, and add a comment to the portfile saying so — otherwise someone will "simplify" it in six months.

What to do instead: keep the dylib, sign it as a nested binary (6.2), and **make sure it's signed before the bundle**. Also verify `install_name_tool` isn't run *after* signing (it is today, at `:180-186`, but signing happens in CI afterwards so it's currently accidental-correct; with 6.2 moving signing into CMake, ordering matters — put the `install_name_tool` command before the codesign commands).

Also note `BackendLibusb.cpp:766-779`: kernel-driver detach fails on macOS without root or an entitlement. There is no such entitlement for a non-DriverKit app. The GameCube adapter will work only if macOS hasn't already claimed the interface. **Leave as-is and log a clear message** — a DriverKit `.dext` is a separate, notarization-gated project and is not worth it.

### 6.5 The self-updater — delete it (S)

`src/resource/update.sh` + `CemuUpdateWindow.cpp:614-624` (`execlp("sh", "sh", <Resources>/update.sh)`). It mounts a DMG and `cp -rf`s over the installed app.

This cannot work under a notarized, quarantined install:
- **App Translocation**: an app launched from a quarantined DMG or from `~/Downloads` runs from a read-only, randomized `/private/var/folders/.../AppTranslocation/` path. `ActiveSettings::GetExecutablePath().parent_path().parent_path()` resolves *there*, so `cp -rf` writes to a translocated copy that vanishes on quit.
- Even installed in `/Applications`, `cp -rf` over a running, signed bundle produces a bundle whose `_CodeSignature` no longer matches the (still-running) process, and the new copy inherits the quarantine flag from the DMG.
- `update.sh` itself has a bug: `APP=$(cd "$(dirname "0")"/;pwd)` — `"0"` should be `"$0"`, so `dirname "0"` is `.` and `APP` is just the CWD.

**Recommendation: delete the in-app updater on macOS entirely.** Delete `src/resource/update.sh`, the copy+chmod at `src/CMakeLists.txt:152-159`, and the `#elif BOOST_OS_MACOS` block at `CemuUpdateWindow.cpp:604-616`. Replace "Check for updates" with a version check that, if newer, opens the releases URL in the browser. Three lines.

**If you later want real in-app updates, the only correct answer is Sparkle** (EdDSA-signed appcast, handles translocation, staged installs, and re-signing correctly). Do not hand-roll it. Note this as a deferred decision.

**Verify:** `find bin/Cemu_release.app -name update.sh` → nothing. Check-for-updates opens a browser.

### 6.6 DMG polish (S)

Move DMG creation out of the workflow YAML into `dist/macos/make_dmg.sh` so it's reproducible locally. Current version is fine functionally (`hdiutil create` + `convert -format UDZO` + `/Applications` symlink). Add: a background image + window geometry via `.DS_Store` (optional, `create-dmg` handles it), and `SetFile -a C` for the custom icon. Low priority.

---

# Phase 7 — Crash / diagnostics

`src/Common/ExceptionHandler/ExceptionHandler_posix.cpp` needs three changes.

### 7.1 `sigaltstack` (S, high value)

Today there is no `sigaltstack` and no `SA_ONSTACK`. Consequence: **a stack-overflow SIGSEGV is unreportable** — the handler needs stack space it doesn't have, so you get an immediate second fault and the process dies with no crash log. Guest fiber stacks are 2 MB with no guard page today (fixed in 1.6), so overflow is a realistic failure mode.

In `ExceptionHandler_Init()`:
```c
static thread_local char s_altStack[SIGSTKSZ * 4];   // SIGSTKSZ is small; arm64 frames are large
stack_t ss = { .ss_sp = s_altStack, .ss_size = sizeof(s_altStack), .ss_flags = 0 };
sigaltstack(&ss, nullptr);
action.sa_flags = SA_SIGINFO | SA_ONSTACK;
```
**`sigaltstack` is per-thread.** Installing it only on the main thread means a crash on `LatteThread` or a guest core still can't report. Add a `RegisterAltStackForThisThread()` call to the `ConfigureThread()` helper from §4.2, so every thread gets one. Cost: 128 KB per thread.

Also: `struct sigaction action;` at `:145` is **uninitialized** — `sa_mask` is set via `sigfillset` and `sa_flags`/`sa_handler` are assigned, but on Darwin `sigaction` has no other members, so this is currently benign. Zero-initialize it anyway (`struct sigaction action{};`).

### 7.2 Demangled backtraces on macOS (S)

`:100-115`: on Linux, `backtrace_symbols` + `DemangleAndPrintBacktrace` writes demangled frames to the crash log. On macOS the `#else` branch does `backtrace_symbols_fd(…, STDERR_FILENO)` — **undemangled, to stderr, not into the crash log file**. A user's crash report is therefore useless.

Fix: write a `DemangleAndPrintBacktraceMac()`. macOS `backtrace_symbols` output format is:
```
3   Cemu  0x0000000104a3f1c8 _ZN10LatteShader7CompileEv + 72
```
i.e. `<idx> <image> <addr> <symbol> + <offset>` — different from the Linux `image(symbol+0xoff) [addr]` format the existing parser expects. Parse on whitespace, take field 3 as the mangled name, run it through `boost::core::demangle` (already used at `:46`), and emit via `CrashLog_WriteLine` so it lands in the log file.

Better still: use **`backtrace_symbols_fmt`** (Darwin-specific, `<execinfo.h>`) with a format string, or skip symbolication entirely and log **image UUID + load address + raw frame addresses**, then symbolicate offline with `atos -o Cemu.app.dSYM/… -l <slide>`. **Recommendation: do both** — log the raw addresses + `_dyld_get_image_header`/UUID (always correct, works with a stripped release binary) *and* the best-effort demangled names (readable for users). With `-fvisibility=hidden` from §1.9 and `-Wl,-dead_strip`, in-process symbolication will be sparse anyway, so the UUID+slide path is the one that actually matters.

Ensure `dSYM` generation: `set(CMAKE_XCODE_ATTRIBUTE_DEBUG_INFORMATION_FORMAT dwarf-with-dsym)` won't apply under Ninja; instead add a `POST_BUILD` `dsymutil $<TARGET_FILE:CemuBin>` for `Release`/`RelWithDebInfo` and archive the `.dSYM` alongside the DMG in CI.

Delete the now-dead Linux-only `#if BOOST_OS_LINUX` branches, `ELFSymbolTable.{h,cpp}`, and the `ARCH_X86_64 && BOOST_OS_LINUX` RIP-recovery at `:97-101`. The arm64 equivalent (recover PC from `((ucontext_t*)context)->uc_mcontext->__ss.__pc`) is **worth adding** — it replaces the deepest backtrace frame with the actual faulting address, which is what you want for a JIT crash:
```c
ucontext_t* uc = (ucontext_t*)context;
backtraceArray[0] = (void*)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
```
(`arm_thread_state64_get_pc` handles PAC stripping — do **not** cast `__pc` directly, pointer authentication will give you a bogus value.)

### 7.3 Mach exception ports — don't (S decision, 0 work)

**Recommendation: do not add Mach exception ports.**

Reasoning: Mach exception handling (`task_set_exception_ports` + a `mach_msg` server thread) is genuinely more powerful than signals — it works when the signal stack is unusable, catches `EXC_BAD_ACCESS` before it's translated, and lets you *resume* the thread with modified state. That last capability is why JITs sometimes want it (e.g. fast-path guest memory access with fault-based bounds checking).

But: this codebase does not do fault-based memory access. `MMURange` uses eager `mprotect`, not demand-faulting. So the only benefit would be crash reporting robustness, and `sigaltstack` (7.1) covers 95 % of that for 20 lines instead of ~400. Additionally, a Mach exception server **conflicts with `lldb`** (whichever registers last wins), so you'd need to detect the debugger and skip it — more complexity.

**Revisit only if** the Metal renderer or a future MMU redesign adopts fault-based guest memory protection. Write this reasoning down.

### 7.4 `DEBUG_BREAK` (S)

`precompiled.h:315` — `#define DEBUG_BREAK raise(SIGTRAP)`. Combined with `sigaction(SIGTRAP, handlerDumpingSignal)` in `ExceptionHandler_Init`, **every `cemu_assert` failure produces a full crash log and `_Exit(1)`**. That's probably intended, but under `lldb` it's hostile: `raise(SIGTRAP)` from a signal-handled process doesn't cleanly break into the debugger.

Change to `__builtin_debugtrap()` (emits `brk #0xF000` on arm64), which `lldb` handles natively and which still raises `SIGTRAP` when no debugger is attached. One-line change, meaningfully better debugging.

---

# Risk register

Ordered by (likelihood × cost), with the early-detection method for each.

| # | Risk | Why likely | Detect early by |
|---|---|---|---|
| **R1** | **`MAP_JIT` single-region limit breaks the recompiler** the moment hardened runtime is enabled. Today's per-function `xbyak` allocations will fail after the first. | Confirmed from Apple docs; current code allocates per function. | **Before** Phase 3: build with `MACOS_BUNDLE=ON` + hardened runtime + ad-hoc signing (6.2) and boot a title. If it dies on the second recompiled function, R1 is confirmed and Phase 3 is a hard blocker, not an optimization. **Do this in Phase 0 — it changes the phase ordering if it fires.** |
| **R2** | **Missing `sys_icache_invalidate` after taking JIT ownership** → intermittent, unreproducible crashes in generated code on M-series only. Worst failure mode in the whole plan. | Zero occurrences in `src/` today; it's hidden inside `xbyak`. Easy to forget when replacing the allocator. | Make `JitCodeArena::Publish()` the *only* path that writes the jump table, and put the invalidate inside it. Add a debug-build counter asserting `publishCount == invalidateCount`. Stress test: force `PPCRecompiler_invalidateRange` in a tight loop for an hour. |
| **R3** | **QoS-induced hang** via `FSpinlock` inversion between a `UTILITY` recompiler and `USER_INTERACTIVE` guest cores. Presents as a total freeze, not a slowdown. | `FSpinlock` is a pure spin with no donation; `recompilerSpinlock` is genuinely shared across those tiers. | **Do §4.3 before §4.2.** Then: run with `taskpolicy -b` (force background) on the recompiler thread and hammer code invalidation. Also `sample $(pgrep Cemu)` during any freeze — a stack pegged in `FSpinlock::lock` is diagnostic. |
| **R4** | **Memory exhaustion on 8 GB.** ~1 GB MEM2 + up to 448 MB eagerly-touched jump table + 164 MB Latte buffer cache + Metal residency, on a machine also running the OS. | Arithmetic in §2.3; the jump-table `memset` loop at `PPCRecompiler.cpp:521-524` forces commit of every reserved page. | Log `footprint`-equivalent (`task_vm_info.phys_footprint`) every 30 s during a play session, plus a boot-time budget line. Watch for `Jetsam` / memory-pressure notifications via `DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`. If it's tight, make the jump table lazily initialized. |
| **R5** | **16 KB page bugs in code you haven't found yet.** §2.3 audits the MMU table, but `MemMapper` is used elsewhere (`PPCRecompiler_init`, `PPCRecompiler_reserveLookupTableBlock`) and any future caller can reintroduce the misalignment. | The existing `FreeMemory` bug went unnoticed for years because the return value is discarded. | Add `cemu_assert` + `cemuLog` on every `mprotect`/`mmap` failure in `MemMapperUnix.cpp`. Add a debug-only startup self-test that round-trips every `MMURange`. Grep for other `mmap`/`mprotect` users — there are currently only two, keep it that way. |
| **R6** | **`ENABLE_VULKAN=OFF` doesn't compile**, blocking Phase 0 before you can build anything. | The option exists but is almost certainly untested; `glslang` is linked unconditionally and `CemuApp.cpp:367` calls `InitializeGlobalVulkan()` inside `#ifdef`. | Attempt the configure+build in the first hour. If it fails, jump straight to §1.1 (delete the renderers) rather than debugging an option you're about to remove. |
| **R7** | **Notarization rejection** on the first CI attempt. | Four independent defects in the current signing invocation (§6). | Run `xcrun notarytool submit --wait` against a *manually* built bundle before wiring CI. `xcrun notarytool log <id>` names the offending binary and reason. Most likely first failure: a nested dylib without hardened runtime. |
| **R8** | **SDL background-thread migration crashes** because `SDL_INIT_VIDEO` gets initialized somewhere and fights `NSApplication`. This is presumably why the wxTimer workaround exists. | `ScreenSaver.h:34-40` explicitly does `SDL_InitSubSystem(SDL_INIT_VIDEO)`. | **Do §5.4 (screensaver → NSProcessInfo) before §5.3 (SDL thread).** Then grep for every `SDL_Init*` call and assert `SDL_WasInit(SDL_INIT_VIDEO) == 0` at startup. |
| **R9** | **Fiber rewrite corrupts guest state** in ways that only manifest as rare, game-specific misbehavior. | Hand-written context switch; the callee-saved register set must exactly match what the AArch64 backend assumes. | Crib the register list from `AArch64GenContext_t::enterRecompilerCode()` (`BackendAArch64.cpp:1628-1650`) rather than from the ABI doc. Test with `PPCREC_FORCE_SYNCHRONOUS_COMPILATION 1` and with `--force-interpreter` to isolate fiber bugs from recompiler bugs. Keep the ucontext path behind a `-DCEMU_LEGACY_FIBER=ON` escape hatch for one release. |
| **R10** | **Metal renderer work (other agent) collides** with the Phase 4 QoS work and the Phase 1 CMake restructure. | Both touch `src/Cafe/CMakeLists.txt` and `MetalPipelineCache.cpp` / `RendererShaderMtl.cpp`. | Land §1.1 (renderer deletion + CMake collapse) **first and fast**, before the Metal agent starts, so they branch from a Metal-only tree. Reserve `ConfigureThread()` in `helpers.h` as the agreed seam for thread QoS and tell them to call it rather than setting priorities inline. |
| **R11** | **`_XOPEN_SOURCE` removal breaks unrelated compilation units** by changing system-header visibility. | It's currently global; removing it *adds* symbols rather than removing them, so this is low-risk — but the reverse (things that relied on the POSIX-strict view) is possible. | Remove it in its own commit. Full clean rebuild. It should only ever add declarations. |
| **R12** | **Removing `FileCache`'s per-write `Flush()` loses cache data.** | It exists because `_Exit()` drops buffered writes; removing it without §5.6's explicit flush-on-exit is a regression. | Do them in the same commit. Test: build a shader cache, quit via the UI, relaunch, verify entry count. Then quit via `kill -TERM` and verify again (the `SIGTERM` handler at `ExceptionHandler_posix.cpp:132-141` also `_Exit`s — it needs the flush too). |

---

## Phase ordering summary

```
P0  bootstrap ──┬─> P1.1 delete GL/VK  (do this first, it unblocks P0)
                └─> [R1 probe: hardened-runtime JIT smoke test]
P1  1.1 → 1.2 → 1.8/1.9 (CMake+target) → 1.3 (x86 purge) → 1.4 (cpu_features)
    → 1.5 (arch macros) → 1.6 (fibers, _XOPEN_SOURCE) → 1.7 (timebase) → 1.10
P2  2.1 (MemMapper) → 2.2 (setEnd) → 2.3 (range table)          [independent of P1]
P6.2 sign-from-CMake                        ← pull forward if R1 fires
P3  JitCodeArena                            ← depends on P6.2 for entitlements
P4  4.1 core counts → 4.3 spinlocks → 4.2 QoS → 4.4 audio → 4.5 dispatch
P5  5.4 screensaver → 5.3 SDL thread → 5.1/5.2 plist+GameMode → 5.5 → 5.6
P7  7.1 sigaltstack → 7.2 backtraces → 7.4 debugtrap
P6  6.1 → 6.3 notarize → 6.4 → 6.5 → 6.6
```

Every arrow is a buildable, runnable checkpoint. The two non-obvious dependencies are **§6.2 before §3** (you can't test `MAP_JIT` without entitlements) and **§4.3 before §4.2** (don't add QoS to unfixed spinlocks) and **§5.4 before §5.3** (kill SDL video before moving SDL off the main thread).

---

## Things I recommend *not* doing

- `-mcpu=apple-m1` / `-march=armv8.5-a` — verified no-op; clang already defaults to `apple-m1` with `+lse +aes +sha2 +dotprod +fullfp16`.
- `-fno-semantic-interposition` — ELF-only concept, meaningless on Mach-O.
- **BOLT** — no Mach-O support in `llvm-bolt`. Not an option, period.
- **Monolithic LTO** — will thrash 8 GB. ThinLTO only.
- **PGO in the default pipeline** — the hot code is JIT-generated and invisible to PGO. Defer, measure, keep only if >3 %.
- **`com.apple.security.cs.jit-write-allowlist`** — inverts the emitter's control flow for zero benefit here, and disables `pthread_jit_write_protect_np`.
- **`com.apple.security.cs.allow-unsigned-executable-memory`** — grants nothing on arm64.
- **Mach exception ports** — `sigaltstack` covers the need for 5 % of the effort; revisit only if you adopt fault-based guest memory.
- **`GameController.framework`** (for now) — better API, but SDL3 also carries the Wiimote HID transport, so you'd maintain both. Fix the wxTimer pump instead. Deferred decision, not a rejected one.
- **Hardware breakpoints in GDBStub** — currently x86+Linux/Windows only; reimplementing on arm64 macOS needs `thread_set_state`/`ARM_DEBUG_STATE64`. Delete and log.
- **Replacing guest fibers with real threads** — multi-month scheduler rewrite.
- **Making libusb static** — LGPL-2.1; keep it dynamic and sign it.
- **Hand-rolling a self-updater** — delete it; Sparkle or nothing.
- **Writing a thread pool** — use libdispatch.
- **Consolidating the PCH** — ~10 s of build time; shrinking `precompiled.h` itself would be the real win, but it's not a Foundation blocker.

---

### Critical Files for Implementation

- `/Users/patricedery/Coding_Projects/Cemu-MacOS/CMakeLists.txt` — deployment target, `CMAKE_OSX_ARCHITECTURES`, renderer option purge, vcpkg overlay simplification, IPO guard, arch gate deletion
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/CMakeLists.txt` — bundle rules, MoltenVK removal, `_XOPEN_SOURCE`, AppleClang warning bug, codesign integration, plist/entitlements wiring
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/util/MemMapper/MemMapperUnix.cpp` — the 16 KB page correctness fix (round base down *and* extend size, check return values)
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/Cafe/HW/Espresso/Recompiler/BackendAArch64/BackendAArch64.cpp` — `AArch64Allocator` replacement, the `JitCodeArena` seam, trampoline generation, `PPCRecompiler_cleanupAArch64Code`
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp` — x86 branch deletion, `PPCRecompilerInstanceData_t` shrink, `deleteFunction` free path, recompiler thread QoS, spinlock
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/util/helpers/helpers.cpp` — `SetThreadName` → `ConfigureThread` (QoS + sigaltstack central seam), `GetPhysicalCoreCount`/`GetPerformanceCoreCount`
- `/Users/patricedery/Coding_Projects/Cemu-MacOS/src/Common/ExceptionHandler/ExceptionHandler_posix.cpp` — `sigaltstack`/`SA_ONSTACK`, macOS demangled backtraces, arm64 PC recovery