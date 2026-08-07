<div align="center">

<img src="docs/assets/tesseraemu-icon.png" width="128" alt="TesseraEmu">

# TesseraEmu

**A Wii U emulator for Apple Silicon Macs.**

arm64 · Metal · macOS 26+

</div>

---

The Wii U eShop closed in 2023. The console was discontinued in 2017, sold badly while it existed,
and holds a library that never came out anywhere else. Hardware fails, discs rot, and nothing is
being manufactured to replace either. Emulation is how these games stay playable.

TesseraEmu exists to make that as good as it can be on one kind of machine: an Apple Silicon Mac.
Not portable, not a compatibility layer, not a compromise. Just Apple Silicon and Metal, treated as
the target rather than as one platform among several.

A *tessera* is the single tile a mosaic is made from. The mark is a **T** laid in tesserae with one
tile still resolving, which is roughly the honest state of any emulator: a picture assembled one
verified piece at a time.

> [!IMPORTANT]
> **This is early.** No releases yet, a handful of titles exercised on one machine, no compatibility
> list. If you want to play something today, [Cemu](https://github.com/cemu-project/Cemu) is the
> mature, well-supported project and you should use it. Come back here when this one has earned it.

## Building it

You need an Apple Silicon Mac (M1 or newer), macOS 26.0 or later, and the Xcode 26 command line
tools.

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja

git clone --recursive https://github.com/PowerBeef/TesseraEmu
cd TesseraEmu

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build

./bin/TesseraEmu_relwithdebinfo -g /path/to/title.wux
```

The first configure builds every dependency from source and takes a while; later builds are about
two minutes.

You also need `keys.txt` in `~/Library/Application Support/TesseraEmu/`, holding the Wii U common key
and the disc key for each title. **Those come from a console you own.** No keys, game code or
copyrighted assets are distributed here, and none will be.

Already using Cemu? On first run your data directory moves from `.../Cemu` to `.../TesseraEmu`:
saves, installed titles, keys and all. Same volume, so it is one atomic rename with no copying. If
it cannot, it leaves your data alone and tells you what to run.

See [BUILD.md](/BUILD.md) for app bundles, code signing and the full flag list.

## What is done so far

**CPU conformance**

- Runs [Andrew Church's `ppc750cl.s`](https://achurch.org/cpu-tests/ppc750cl.s) — silicon-validated
  against real Espresso — as Wii U homebrew. No game image, console, keys or SDK required.
- **Values-only suite (`IGNORE_FPSCR_STATE=1`): 0 failures**, recompiler and interpreter identical.
  Every wrong *result* the suite can see under that build is fixed (FP, paired-single, integer).
- **Full suite with FPSCR state: 928 failures**, also arms identical — residual is FPSCR
  bookkeeping (FPRF, FI/FR, exception stickies), not wrong answers. First landing was 1,030
  (354 values + 676 FPSCR state); values are closed. See [`testing/cpu-tests/`](/testing/cpu-tests/).

**Performance**

- Removed three idle spins in the scheduler and command processor. Mario Kart 8 went from 184% to
  104% of one core at an identical locked 60 FPS.
- Parked a guest fence wait that was being spun on 65 million times a second, worth a further 20% of
  process CPU in Breath of the Wild.
- Thread QoS by role: guest cores and the GPU command thread run `USER_INTERACTIVE`, compilation
  runs `UTILITY` and is sized against the efficiency cluster rather than total core count.
- `FSpinlock` is now `os_unfair_lock`, so the kernel knows who holds a lock on an asymmetric CPU.
- Title decryption uses the ARMv8 AES instructions instead of a software table.
- Audio mixing moved to a lock-free ring, off a mutex that was being taken on CoreAudio's realtime
  thread.

**Correctness**

- Fixed a constructor writing 248 bytes past the end of an array on every startup, which had been
  papered over with a `// HACK: ... garbage data` comment for years.
- Fixed geometry-shader texture bindings writing past the end of the renderer's texture array.
- Fixed graphic packs with custom output shaders crashing on boot, and writing a pipeline pointer
  out of bounds once per frame.
- Fixed guest memory ranges failing to unmap on 16 KB pages, which left them writable across a title
  switch.
- Fixed the JIT invalidating only part of its generated code, a stale-instruction hazard on cold
  paths.
- Fixed sampler LOD bias never being applied, and graphic packs corrupting GPU registers.
- Draws that sample the surface they are rendering into now split the render pass, including when
  the two are separate texture objects over the same memory.
- A shader stage that fails to compile now drops its draw instead of aborting the emulator.
- Guest FP and paired-single semantics brought into line with Espresso on the values-only suite
  (quantize, VE/ZE, FMA rounding, frsp/fctiw, merge excess-range, frsqrte denorms, lfd/PS hazards).

**Platform**

- Native `GameController.framework` input, screensaver inhibition through `NSProcessInfo`, SDL input
  off the main thread, and Carbon removed.
- `NSHighResolutionCapable`, without which macOS handed Metal a half-resolution drawable.
- Crash reports with real symbolicated arm64 backtraces, and a signal handler that survives stack
  overflow.
- Code signing and entitlements handled by the build rather than by hand.
- arm64-only, Metal-only, macOS 26.0 minimum — portability deliberately removed so the binary links
  no GL/Vulkan/MoltenVK and no x86 path.

Details and measurements for all of it are in [`docs/status/`](/docs/status/). There is also a
[hardware reference](/docs/hardware/) and a set of [test ROMs](/testing/) that need no game image.

## Credit

TesseraEmu is a fork of [Cemu](https://github.com/cemu-project/Cemu), which is the work of many
people over many years and is the reason this project could exist at all. The Metal renderer it
builds on was contributed to Cemu by SamoZ256. `LICENSE.txt` is untouched, the About dialog still
credits Exzap and Petergov, and the copyright line reads *"Cemu Project and TesseraEmu
contributors"*, extended rather than replaced.

This project is **not affiliated with or endorsed by the Cemu project**, and carries its own name
because MPL-2.0 grants no trademark rights. Licensed under [MPL-2.0](/LICENSE.txt).

> [!NOTE]
> **This project is AI-assisted development.** Cemu asks that contributed code be written and
> understood by a human, for good reasons. **Do not submit these changes upstream as-is.**

Wii and Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo.
