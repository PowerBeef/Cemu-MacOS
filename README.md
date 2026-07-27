# TesseraEmu — Wii U emulator, native to Apple Silicon

TesseraEmu is a hard fork of [Cemu](https://github.com/cemu-project/Cemu), retargeted exclusively at **Apple Silicon and macOS 26**. It is not affiliated with or endorsed by the Cemu project. Licensed under MPL-2.0, as Cemu is — see [LICENSE.txt](/LICENSE.txt).

Upstream Cemu is portable across Windows, Linux and macOS, and its macOS build has historically shipped with a disclaimer about "degraded performance due to the use of MoltenVK and Rosetta for ARM Macs". This fork drops portability in order to remove exactly those compromises: it is arm64-only, Metal-only, and targets a single OS version.

> **Status: early.** The emulator builds, boots and runs. It has been verified against one commercial title on one machine (Apple M2, macOS 26.5.2). Broad game compatibility has not been tested and should not be assumed.

## What is different from upstream

| | Upstream Cemu | This fork |
|---|---|---|
| Architectures | x86-64 + arm64 | **arm64 only** |
| Renderers | OpenGL, Vulkan/MoltenVK, Metal | **Metal only** |
| Default renderer on macOS | Vulkan via MoltenVK | **Metal** |
| Minimum macOS | 13.4 | **26.0** |
| Thread scheduling | none | **QoS-aware, P/E-core split** |

Roughly 56,000 lines removed: both non-Metal renderers, the GLSL shader emitter, the entire x86-64 recompiler backend, glslang, MoltenVK and the Vulkan headers.

### Notable changes

- **Metal renderer heap corruption fixed.** The constructor initialised a `[3][31]` array with a loop bound of 4, writing 248 bytes past the end. This was the cause of a long-standing `// HACK: for some reason, this variable ends up being initialized to some garbage data` workaround, now removed.
- **ARMv8 AES.** Title and content decryption used a table-driven software implementation on arm64. Now uses the crypto extensions (`AESE`/`AESD`), validated against FIPS-197.
- **16 KB page correctness.** `MemMapper` rounded a mapping's base but not its length, and did no rounding at all when unmapping, so releasing a guest range failed silently with `EINVAL` and left it writable across a title switch.
- **JIT I-cache flush fixed.** The AArch64 backend left the emitter cursor short of the end of generated code, so `readyRE()` invalidated only part of it — a stale-instruction hazard on cold paths.
- **`PPCInterpreter_t` and the recompiler** stripped of x86-only state; the AArch64 backend's `static_assert`s validate the resulting layout at compile time.
- **Audio** no longer takes a mutex on CoreAudio's realtime thread (an unfixable priority inversion); replaced with a lock-free SPSC ring.
- **Thread QoS.** The three guest cores and the GPU command thread run at `USER_INTERACTIVE`; recompilation and shader compilation at `UTILITY`. Compile pools are sized against the efficiency cluster instead of total core count, which previously spawned up to 17 threads on a 4P+4E machine.
- **Screensaver inhibition** reimplemented with `NSProcessInfo`; it was disabled on macOS because the SDL-based version initialised `SDL_INIT_VIDEO` inside a wx app that already owns `NSApplication`.
- **SDL input** moved off a 5 ms main-thread timer onto a dedicated thread, as on every other platform.
- **`NSHighResolutionCapable`** added — without it macOS ran the app magnified and created the Metal drawable at half resolution.

## Requirements

- Apple Silicon Mac (M1 or newer)
- macOS 26.0 or later
- Xcode 26 command line tools

## Building

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja

git submodule update --init --depth 1 \
  dependencies/{vcpkg,ZArchive,cubeb,imgui,metal-cpp,xbyak_aarch64}

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build
```

The first configure builds all dependencies from source and takes a while; subsequent builds are around two minutes on an M2.

MoltenVK is **not** required — Vulkan is gone.

For a signed `.app` bundle, use `-DMACOS_BUNDLE=ON`. Bundle builds sign themselves ad-hoc with the hardened runtime and the JIT entitlement, which is all that is needed for the recompiler to run; **no Apple Developer Program membership is required** for local development. Override `CEMU_CODESIGN_IDENTITY` for a distributable build.

See [BUILD.md](/BUILD.md) for more detail and [CLAUDE.md](/CLAUDE.md) for architecture notes and the verification workflow.

## Running

TesseraEmu needs `keys.txt` in its data directory (`~/Library/Application Support/TesseraEmu/`) containing the Wii U common key and the disc key for each title. These are derived from your own console and are not distributed here.

```sh
./bin/TesseraEmu_relwithdebinfo --verbose -g /path/to/title.wux
```

## Design notes

The full research and staged plan live in [`docs/porting/`](/docs/porting/): a master plan with a risk register, plus detailed designs for the platform, CPU/JIT and graphics workstreams. They record what was measured, and where measurement contradicted the plan — for example Apple's documented one-`MAP_JIT`-region-per-process limit turns out not to be enforced on macOS 26, which downgraded a supposed blocker to an optional optimisation.

Standalone probes under [`tools/probes/`](/tools/probes/) re-verify the platform behaviour those decisions rest on, so they can be re-checked on a future OS rather than re-argued.

## Relationship to upstream

This is a hard fork and diverges deliberately. Bug fixes here that are not arm64- or Metal-specific may be worth porting upstream, but the deletions are not.

**This fork's changes were written with AI assistance (Claude).** Upstream Cemu's contribution policy asks that submitted code be written and understood by a human, and explains why — reviewing capacity, and concerns about LLM-generated emulation logic being plausible but inaccurate. That policy is reasonable and this repository does not attempt to work around it: **do not submit these changes upstream as-is.** Anything worth contributing back should be re-derived and written by a person who understands it.

## License

TesseraEmu, like the Cemu code it derives from, is licensed under the [Mozilla Public License 2.0](/LICENSE.txt). MPL-2.0 grants no trademark rights, which is why this fork carries its own name rather than Cemu's; copyright in the inherited code remains with its authors and the licence notices are unchanged. Files in the `dependencies` directory are covered by the licenses of the original code, as are some individual files in `src` where noted in their headers.

## Upstream links

- [Cemu](https://github.com/cemu-project/Cemu) · [Website](https://cemu.info) · [Compatibility wiki](https://wiki.cemu.info/wiki/Main_Page) · [Discord](https://discord.gg/5psYsup)
