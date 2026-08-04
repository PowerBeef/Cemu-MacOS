<div align="center">

<img src="docs/assets/tesseraemu-icon.png" width="128" alt="TesseraEmu">

# TesseraEmu

**A Wii U emulator built for Apple Silicon rather than ported to it.**

arm64 only · Metal only · macOS 26+

</div>

---

TesseraEmu is a hard fork of [Cemu](https://github.com/cemu-project/Cemu) with one goal: to be the
best way to run Wii U games on a Mac. Cemu is cross-platform and reaches Metal on macOS through
MoltenVK. This fork gives up every other platform so it can talk to Metal directly, and so it can
assume 16 KB pages, an asymmetric CPU and unified memory instead of carrying fallbacks.

> [!IMPORTANT]
> **Use upstream Cemu today.** This fork is early: no releases, two titles verified on one machine,
> and no compatibility claims. Cemu has years of compatibility work behind it and will actually run
> your game. Changing that answer is the point of the project, but it is not today.

## Getting started

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
two minutes. **Do not install MoltenVK.** It is not a dependency.

You also need `keys.txt` in `~/Library/Application Support/TesseraEmu/`, holding the Wii U common key
and the disc key for each title. Those come from your own console and are not distributed here.

**Coming from Cemu?** On first run your data directory is moved from `.../Cemu` to
`.../TesseraEmu`: saves, installed titles, keys and all. Same volume, so it is a single atomic
rename with no copy. If it fails it leaves your data untouched and tells you what to run.

See **[BUILD.md](/BUILD.md)** for app bundles, code signing and the full flag list.

---

## What this fork changes

The Metal renderer itself came from upstream, contributed to Cemu by SamoZ256 in
[PR #1287](https://github.com/cemu-project/Cemu/pull/1287). Everything below starts from that work.

**Written for this hardware.** These need a single-platform target and do not apply to Cemu as-is.

- **Metal directly, with no translation layer.** OpenGL, Vulkan, MoltenVK, glslang and the GLSL
  shader emitter are gone, along with the entire x86-64 recompiler backend. There is no runtime
  backend selection because there is nothing left to select.
- **Thread QoS and a P/E-core split.** Guest cores and the GPU command thread run
  `USER_INTERACTIVE`; shader and code compilation run `UTILITY` and are sized against the efficiency
  cluster. The previous formula spawned up to 17 threads on a 4P+4E machine, all competing for the
  cores emulation needed.
- **ARMv8 AES.** Title decryption used a software table implementation on arm64. It now uses the
  crypto extensions, validated against the FIPS-197 vectors.
- **Realtime-safe audio.** The mixer took a mutex on CoreAudio's realtime thread. That is replaced
  with a lock-free ring, and `FSpinlock` became `os_unfair_lock` so the kernel knows who holds a lock
  on an asymmetric CPU.
- **Native integration.** `GameController.framework` input, `NSProcessInfo` screensaver inhibition,
  SDL input off the main thread, and `NSHighResolutionCapable` (without it macOS handed Metal a
  half-resolution drawable).

**Bugs fixed.** Most of these are not Apple-specific and exist upstream too.

- A constructor initialised a `[3][31]` array with a loop bound of `4`, writing **248 bytes past the
  end** on every startup. It was the real cause of a long-standing `// HACK: ... garbage data`
  workaround, now deleted with the bug.
- A geometry-shader texture binding wrote past the end of the renderer's texture array, because the
  stage bases are strided by 32 and the array was sized for 54 entries.
- Any graphic pack shipping an output shader **crashed the emulator on every boot**, and wrote a
  pipeline pointer out of bounds once per frame on the way there.
- `MemMapper` rounded a mapping's base but not its length, so releasing a guest range failed
  silently and left it writable across a title switch. Apple Silicon's 16 KB pages make this
  reachable where 4 KB pages hid it.
- The AArch64 backend left the emitter cursor short of the end of generated code, so only part of it
  was invalidated: a stale-instruction hazard on cold paths.
- Sampler LOD bias was never applied, and graphic packs corrupted GPU registers.
- A draw that samples the surface it is rendering into now splits the render pass instead of reading
  undefined data, including when the two are separate texture objects over the same memory.

**Faster, where it was measured.** Every number here was taken on one 8 GB M2 Mac mini against each
title's own target frame rate.

- Removing three idle spins cut Mario Kart 8 from **184% to 104% of one core** at an identical
  60 FPS.
- Buffer-cache uploads no longer tear down the render pass. `Auto` storage resolves to shared, so
  an upload is a memcpy rather than a staging blit, and mid-frame uploads stopped splitting passes.
- Breath of the Wild's open world runs at **20 fps because of one config default**, not because of
  the renderer. Turning off `GX2DrawdoneSync` gives **30.06 fps**, which is the title's actual
  target. It is a documented accuracy tradeoff, so the default is unchanged and both numbers are
  published.

**Measured rather than guessed.** The fork ships a telemetry harness compiled into the core, a
homebrew test ROM suite that needs no game image, and a
[status page](/docs/status/) recording every item attempted since the fork point, including the ones
that were refuted, cancelled or reverted. Roughly half the optimisation ideas on that page did not
work, and why they failed is the useful part.

---

## Going deeper

| | |
|---|---|
| [`docs/status/`](/docs/status/) | every item tried and what it measured, generated from the repo |
| [`docs/hardware/`](/docs/hardware/) | a nine-chapter reference on the Wii U's silicon, every claim provenance-tagged |
| [`docs/porting/`](/docs/porting/) | staged plans, risk register, per-workstream designs |
| [`testing/`](/testing/) | golden-scene capture, the telemetry differ, and the test ROMs |

## Credit and licence

TesseraEmu is **not affiliated with or endorsed by the Cemu project**, and carries its own name
because MPL-2.0 grants no trademark rights. `LICENSE.txt` is untouched, the About dialog still
credits Exzap and Petergov, and the bundle copyright reads *"Cemu Project and TesseraEmu
contributors"*, extended rather than substituted. Licensed under
[MPL-2.0](/LICENSE.txt), like the Cemu code it derives from.

> [!NOTE]
> **This fork's changes were written with AI assistance (Claude).** Upstream Cemu asks that submitted
> code be written and understood by a human, for good reasons. **Do not submit these changes upstream
> as-is.** Anything worth contributing back should be re-derived by a person who understands it.

Wii and Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo, and no game
code, keys or copyrighted assets are distributed here.

[Cemu](https://github.com/cemu-project/Cemu) · [Website](https://cemu.info) · [Compatibility wiki](https://wiki.cemu.info/wiki/Main_Page) · [Discord](https://discord.gg/5psYsup)
