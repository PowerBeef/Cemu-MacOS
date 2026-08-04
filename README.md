<div align="center">

<img src="docs/assets/tesseraemu-icon.png" width="128" alt="TesseraEmu">

# TesseraEmu

**A Wii U emulator built for Apple Silicon rather than ported to it.**

arm64 only · Metal only · macOS 26+

</div>

---

A *tessera* is the single tile a mosaic is assembled from. The mark is a **T** laid in tesserae, with
one tile still resolving, which is roughly the honest state of any emulator: a picture built one
verified piece at a time.

TesseraEmu is a hard fork of [Cemu](https://github.com/cemu-project/Cemu), and it keeps the parts
that make Cemu good: the PowerPC recompiler, the Latte GPU emulation, and the Cafe OS HLE layer.
What it gives up is portability. Cemu is cross-platform by design, supports three renderer backends,
and reaches Metal on a Mac through MoltenVK. This fork targets one platform and talks to Metal
directly.

That is a narrower goal, not a better one. Portability is a real feature and this fork abandons it
completely. The bet is that dropping it buys enough room to fix things that are awkward to fix while
staying portable, and to use platform APIs that have no cross-platform equivalent.

> [!IMPORTANT]
> **Status: early.** It builds, boots and plays. It has been verified against two commercial titles,
> Mario Kart 8 and Breath of the Wild, on exactly one machine: an **8 GB M2 Mac mini** running
> macOS 26.5. Broad game compatibility has **not** been tested and should not be assumed. There are
> no releases yet; build it yourself.

### What that means in practice

- **Apple Silicon only.** `precompiled.h` raises an `#error` on any other target, and the entire
  x86-64 recompiler backend is deleted.
- **Metal only.** OpenGL, Vulkan, MoltenVK, glslang and the GLSL shader emitter are gone. There is
  no runtime backend selection because there is nothing left to select.
- **macOS 26.0 minimum**, which is what lets the code use current Metal and scheduling APIs without
  carrying fallbacks for older systems.

The status page carries the current diffstat, derived from the repository rather than typed here.

### Which one should you use?

**Use upstream Cemu.** It runs on Windows, Linux and macOS, it has years of compatibility work and a
large community behind it, and it is the project that will actually help you play a game today.

This fork is worth a look if you are on an Apple Silicon Mac and are specifically interested in what
a single-platform build measures differently, or if you want to read the hardware reference and the
measurement notes below. It is a research-flavoured fork, not a replacement.

---

## Getting started

**You need** an Apple Silicon Mac (M1 or newer), macOS 26.0 or later, and the Xcode 26 command line
tools.

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja

git clone --recursive https://github.com/PowerBeef/TesseraEmu
cd TesseraEmu

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build
```

The first configure builds every dependency from source and takes a while. Incremental builds are
around two minutes on an M2. MoltenVK is not a dependency; do not install it.

```sh
./bin/TesseraEmu_relwithdebinfo -g /path/to/title.wux
```

TesseraEmu needs `keys.txt` in `~/Library/Application Support/TesseraEmu/`, holding the Wii U common
key and the disc key for each title. These come from your own console and are not distributed here.
Coming from Cemu? Your data directory is migrated automatically and atomically on first run
([details below](#coming-from-cemu)).

See **[BUILD.md](/BUILD.md)** for app bundles, code signing and the full flag list.

---

## What actually changed

Every item below is a specific defect found and fixed, or a platform capability the portable
codebase had no straightforward way to use.

### Bugs fixed along the way

- **Metal renderer heap corruption.** The constructor initialised a `[3][31]` array with a loop bound
  of `4`, writing **248 bytes past the end** on every startup. This turned out to be the real cause
  of a long-standing `// HACK: for some reason, this variable ends up being initialized to some
  garbage data` workaround, now deleted along with the bug.
- **16 KB page correctness.** `MemMapper` rounded a mapping's base but not its length, and did no
  rounding at all when unmapping, so releasing a guest range failed silently with `EINVAL` and left
  it writable across a title switch. Apple Silicon's 16 KB pages make this reachable where 4 KB pages
  hid it.
- **JIT I-cache flush.** The AArch64 backend left the emitter cursor short of the end of generated
  code, so `readyRE()` invalidated only part of it. A stale-instruction hazard that surfaces only on
  cold paths, which is the worst kind.
- **Graphic packs corrupting GPU registers**, and sampler LOD bias never being applied at all.

### Written for this hardware

- **ARMv8 AES.** Title and content decryption ran a table-driven software implementation on arm64.
  It now uses the crypto extensions (`AESE`/`AESD`), validated against the FIPS-197 vectors by a
  standalone probe.
- **Thread QoS, and a P/E-core split.** The three guest cores and the GPU command thread run at
  `USER_INTERACTIVE`; recompilation and shader compilation at `UTILITY`. Compile pools are sized
  against the efficiency cluster rather than total core count. The previous formula spawned up to
  **17 threads on a 4P+4E machine**, every one of them competing for the P-cores emulation needed.
- **`FSpinlock` became `os_unfair_lock`.** A raw spinlock on an asymmetric-core CPU can burn a P-core
  spinning against a descheduled E-core holder. `os_unfair_lock` tells the kernel who owns the lock.
- **Realtime-safe audio.** The mixer took a mutex on CoreAudio's realtime thread, an unfixable
  priority inversion and a glitch source. It was replaced with a lock-free SPSC ring, plus a latency
  request above the device's own floor.
- **Three idle spins removed.** The scheduler stopped burning a P-core on clock reads, and the GPU
  command thread now parks on `wfe` instead of spinning for ring-buffer data. Together those are
  **1.77x less total process CPU** on Mario Kart 8 (183% to 104% of one core) at an identical 60 FPS.
  The third was a busy-wait on a single guest fence, cut a further **20%** in Breath of the Wild, and
  is [below](#measuring-instead-of-guessing).
- **Native macOS integration.** Screensaver inhibition through `NSProcessInfo` (the SDL version was
  disabled on macOS because it initialised `SDL_INIT_VIDEO` inside a wx app that already owns
  `NSApplication`); SDL input moved off a 5 ms main-thread timer onto its own thread; a native
  `GameController.framework` input provider; Carbon dropped.
- **`NSHighResolutionCapable`.** Without it, macOS ran the whole app magnified and handed Metal a
  half-resolution drawable.

---

## Measuring instead of guessing

Optimisation work is easy to do on intuition and hard to do on evidence. This fork ships a telemetry
harness compiled into the core: counters across CPU, GPU, memory and accuracy, `__thread`-local and
cache-line striped, gated behind a single branch on a global mask so that a disabled build costs
nothing measurable. The counter total lives on the status page, derived from the source rather than
typed, because a typed count is exactly the sort of number that goes stale.

```sh
./bin/TesseraEmu_relwithdebinfo -g title.wux --telemetry after.jsonl --telemetry-label after
python3 testing/telemetry-report.py before.jsonl after.jsonl     # aligned by counter name
```

It earns its keep by contradicting things that seemed obvious:

- The open world was assumed GPU-bound. It is not. Guest cores sit **~18% busy** and the GPU **~38%**,
  and the frame time is **vsync-quantised**: 49.90 ms is three display refreshes, so a small increase
  in work costs a whole one-refresh step rather than a proportional slowdown.
- Chasing *what* misses that deadline went through the renderer twice and found nothing. Neither the
  command-buffer submission rate nor the GPU-side ordering moved between a 20 fps and a 30 fps frame.
  Measuring the frame's **critical path** directly instead, from first command buffer to last pixel,
  put it at **35.25 ms against a 33.27 ms deadline**, and named what pushes it over: an emulated
  `GX2DrawDone` that synchronously drains *every* in-flight texture readback, twice a frame, whether
  the game asked for one or not. It blocks for 6.8 ms and idles the GPU for 16.4 more. Without it the
  critical path is **18.64 ms** and **Breath of the Wild goes from 20.04 to 30.06 fps**, which is its
  actual target. It had been mistaken for a renderer problem for the entire investigation.
- That frame time was traced to a **single guest fence** at one address, spun on **65 million times
  per second**. Parking it removed **99.6% of the spins** and a fifth of all CPU, and moved the frame
  rate not at all, which is itself the result.
- Breath of the Wild calls **31 distinct unimplemented imports**, and **91.7% of that volume was one
  function**: `GX2SetAlphaToMaskReg`, 2.76 million calls in a single run. It looked like the largest
  compatibility gap in the tree. Implementing it and then counting *draws that enable the feature*
  rather than *calls to the setter* gives **zero, across 9,181 frames**. The game asks for the state
  three hundred times a frame and never once turns it on. The renderer work was retired unbuilt.
- A hand-written fibre-switch routine is **70x faster** than the `swapcontext` the guest scheduler
  uses today (454 ns to 6.5 ns, measured). It would also not move the frame rate, because the
  1.4 ms/frame it saves comes out of a stage that already has 20 ms of slack. Measuring the win and
  measuring whether the win matters are different questions.
- Two rounds of tile-based-deferred-rendering optimisations were designed, measured and **rejected**.
  An earlier conclusion turned out to have been measured on the wrong scene, and is recorded as a
  correction rather than quietly deleted.

Every measurement lands in `testing/golden/baseline.tsv`, alongside a window-only screenshot:

| Scene                          | Commit    | FPS       |
| ------------------------------ | --------- | --------- |
| Mario Kart 8, title            | `213bcd7` | 60.00     |
| BotW, shrine interior          | `2c09604` | 28.63     |
| BotW, Korok Forest             | `5933733` | 20.05     |
| BotW, Korok Forest (sync off)  | `e34facb` | **29.81** |

The last row is the same scene with **`GX2DrawdoneSync`**, a user-facing accuracy option that is on
by default, turned off. It is listed separately rather than replacing the row above it, because the
20 fps figure is what the emulator does out of the box and both numbers are real.

The obvious fix was to make that drain surgical, waiting only for what the guest actually reads,
rather than to change the default. **Three attempts later, that fix does not exist.** The
delayed-start theory, the position-in-command-buffer theory and the GPU-ordering theory were each
instrumented and each came back empty. The last was a full A/B at n=3 per arm in which every
performance range overlapped. What the emulator waits for is the GPU genuinely still having ~6 ms of
work outstanding, which is what `GX2DrawDone` means. Only doing less GPU work can shorten it, and
that is where the effort goes next.

Everything above was measured on one machine, an **8 GB M2 Mac mini** (4P+4E, 16 KB pages,
macOS 26.5), against each title's own target frame rate: 60 FPS for Mario Kart 8, 30 for Breath of
the Wild. Numbers from a different Apple Silicon part are not comparable to these.

Those open-world numbers are not good yet, and they are published rather than hidden. The harness
exists so that the next change to them is attributable.

---

## The hardware reference

[`docs/hardware/`](/docs/hardware/) is a 16,000-word, nine-chapter reference on the Wii U's silicon
and system software: the Espresso tri-core PowerPC 750CL at 1,243.125 MHz with its paired singles and
locked cache, Latte's R700-derived GPU7, the GX2 write-gather to PM4 to ring-buffer command path,
Cafe OS's *cooperative* scheduler, the audio DSP, and a register of every known accuracy gap.

Every claim carries a provenance tag: `[SRC file:line]` for something read out of this codebase, all
of which are machine-verified by `tools/docs/check-src-refs.py`; `[HW]` for a hardware datasheet;
`[RE]` for community reverse engineering; `[EST]` for an estimate; and `[CONFLICT]` where sources
disagree. Writing it found and fixed two real bugs.

[`tools/probes/`](/tools/probes/) holds standalone programs that re-verify the platform behaviour
those decisions rest on, so a future macOS can be re-checked rather than re-argued. One has already
paid for itself: Apple's documented one-`MAP_JIT`-region-per-process limit turns out not to be
enforced on macOS 26, which downgraded a supposed blocker to an optional optimisation.

---

## Repository layout

| Path                     | What lives there                                                |
| ------------------------ | --------------------------------------------------------------- |
| `src/Cafe/HW/Espresso/`  | PowerPC interpreter and the AArch64 recompiler                  |
| `src/Cafe/HW/Latte/`     | GPU emulation and the Metal renderer                            |
| `src/Cafe/OS/`           | Cafe OS HLE: `coreinit`, `gx2`, `nn_*`                          |
| `src/Cemu/Telemetry/`    | the counter harness                                             |
| `docs/hardware/`         | the hardware reference                                          |
| `docs/porting/`          | staged plans, risk register, per-workstream designs             |
| `docs/status/`           | the live status tracker: every item tried, and its result       |
| `docs/testing/`          | test strategy, provenance, and what is actually verified        |
| `tools/probes/`          | standalone platform-behaviour probes                            |
| `tools/icon/`            | renders the app icon from its SVG, reproducibly                 |
| `testing/`               | golden-scene capture, telemetry differ, baselines               |
| `testing/cpu-tests/`     | PowerPC 750CL conformance suite, the first CPU accuracy signal  |
| `testing/rom-tests/`     | named Cafe OS and GX2 assertions with recorded expectations     |
| `testing/graphics-tests/`| render-pass self-dependency reproducer                          |
| `testing/toolchain/`     | builds devkitPPC and wut, including with no root                |

`docs/status/index.html` is a generated, self-contained page tracking every item attempted on this
fork and what it measured, including the ones that were refuted, cancelled or reverted. Open it
directly; regenerate it with `python3 docs/status/build-status.py`. Its commit list, diffstat,
baseline table and counter totals are read from the repository rather than typed, so those cannot go
stale.

CI builds, signs, verifies and smoke-tests a real `.app` on every push. Internal identifiers still
say `Cemu` (`cemuLog_*`, `CemuConfig`, the `CemuCafe` target) deliberately: renaming them would be
thousands of lines of churn for no user-visible gain.

---

## Coming from Cemu

On first run, `~/Library/Application Support/Cemu` is moved to `.../TesseraEmu`: saves, installed
titles, `keys.txt` and all. Source and destination sit on the same APFS volume, so it is a single
atomic `rename`, with no copy and no window in which the data exists in neither place. If the rename
fails it does **not** fall back to copying. It leaves your data untouched and shows you the exact
command to run. A failed rebrand is an inconvenience; a half-copied 5 GB NAND is data loss.

---

## Relationship to Cemu

This is a hard fork and it diverges deliberately. Bug fixes here that are not arm64- or
Metal-specific may be worth porting upstream; the deletions are not.

TesseraEmu is **not affiliated with or endorsed by the Cemu project.** It carries its own name
precisely because MPL-2.0 grants no trademark rights, and a modified build should not present itself
as someone else's project. Credit does not move: `LICENSE.txt` is untouched, the About dialog still
credits Exzap and Petergov and links upstream, and the bundle copyright reads *"Cemu Project and
TesseraEmu contributors"*, extended rather than substituted.

The Metal renderer this fork builds on was contributed to Cemu by SamoZ256 in
[PR #1287](https://github.com/cemu-project/Cemu/pull/1287). Every Metal improvement described above
starts from that work.

> [!NOTE]
> **This fork's changes were written with AI assistance (Claude).** Upstream Cemu's contribution
> policy asks that submitted code be written and understood by a human, and explains why: reviewing
> capacity, and the risk of LLM-generated emulation logic being plausible but inaccurate. That policy
> is reasonable and this repository does not try to work around it. **Do not submit these changes
> upstream as-is.** Anything worth contributing back should be re-derived and written by a person who
> understands it.

## License

TesseraEmu, like the Cemu code it derives from, is licensed under the
[Mozilla Public License 2.0](/LICENSE.txt). Copyright in the inherited code remains with its authors
and the licence notices are unchanged. Files in `dependencies/` are covered by the licences of the
original code, as are individual files in `src/` where noted in their headers.

Wii and Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo, and no game
code, keys or copyrighted assets are distributed here.

## Upstream links

- [Cemu](https://github.com/cemu-project/Cemu) · [Website](https://cemu.info) · [Compatibility wiki](https://wiki.cemu.info/wiki/Main_Page) · [Discord](https://discord.gg/5psYsup)
