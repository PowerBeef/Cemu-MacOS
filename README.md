<div align="center">

<img src="docs/assets/tesseraemu-icon.png" width="128" alt="TesseraEmu">

# TesseraEmu

**A hard fork of Cemu — Apple Silicon and Metal only.**

arm64 · Metal · macOS 26+

</div>

---

The Wii U eShop closed in 2023. The console was discontinued in 2017. Its library includes games that
never shipped anywhere else. Hardware fails, discs rot, and nothing is being made to replace either.
Emulation is how those titles stay playable.

TesseraEmu is not a portable emulator with a Mac backend bolted on. It is a **deliberate hard fork**
of [Cemu](https://github.com/cemu-project/Cemu) that deletes every other target: **arm64 only**,
**Metal only**, **macOS 26.0 minimum**. Three graphics backends became one; two host architectures
became one. The binary links Metal and QuartzCore for drawing — not OpenGL, not Vulkan, not MoltenVK.

A *tessera* is one tile in a mosaic. The mark is a **T** laid in tesserae with one tile still
resolving: accuracy and performance are assembled one measured piece at a time, and the unfinished
tile is left on purpose.

> [!NOTE]
> **No numbered releases yet.** Day-to-day play is still early (no public compatibility list; a
> handful of titles exercised hard on one machine). If you need a mature multi-platform player
> today, use [Cemu](https://github.com/cemu-project/Cemu). This repo is for people who care that the
> guest CPU and graphics path are *measured*, not just that games boot.

## Why a hard fork

Upstream Cemu is portable by design. Portability has a cost: every codegen and renderer decision is
mediated by `#ifdef`s and the lowest common denominator. TesseraEmu takes the opposite bet:

| constraint | consequence |
|---|---|
| arm64 only | One JIT backend (`BackendAArch64`); x86 IML machinery deleted |
| Metal only | Latte → MSL at runtime; no GLSL emitter, no MoltenVK |
| macOS 26+ | APIs and page size (16 KB) assumed, not probed at runtime |

If something looks like it needs a runtime arch or backend check, it doesn't — see
[`AGENTS.md`](AGENTS.md) and [`docs/porting/`](docs/porting/).

## What is verified

Numbers below appear in [`docs/status/`](docs/status/) (the ledger). Do not invent new ones in
prose without recording them there.

### Guest CPU — Espresso conformance

TesseraEmu runs [Andrew Church's `ppc750cl.s`](https://achurch.org/cpu-tests/ppc750cl.s) — a
public-domain PowerPC suite **validated by its author against real Espresso silicon** — as ordinary
Wii U homebrew. No game image, console, keys, or SDK required.

| run | failures |
|---|---:|
| Recompiler, full FPSCR state | **0** |
| Interpreter, full FPSCR state | **0** |
| Unique to either arm | **0** |
| Either arm, values only (`IGNORE_FPSCR_STATE=1`) | **0** |

That is every instruction the suite checks — including all paired-single `ps_*` / `psq_*` ops —
matching Espresso on **results and FPSCR bookkeeping** (FPRF, FI, exception stickies), with the
JIT and the interpreter in lockstep.

First landing was **1,030** failures (354 wrong values + 676 FPSCR state). Values closed first;
full suite then **928 → 0**. How (shared helpers, PS defer, host IXC traps, FMA residual, TwoSum
fadd): [`docs/testing/fpscr-suite-green.md`](docs/testing/fpscr-suite-green.md).

### Performance (measured, not vibes)

| | |
|---|---|
| **Mario Kart 8** | Locked **60 fps** at **~104% of one core** (was **~184%** before idle-spin fixes) |
| **Breath of the Wild** (Korok Forest) | **20.04 fps** default; **30.06 fps** with `GX2DrawdoneSync` off — a config accuracy tradeoff, not a renderer mystery |
| **GPU limiters** (Korok, active samples) | **45.6%** ALU-bound; texture sampling 22.8%; the whole memory path ~12.8% |

Idle spins in the scheduler/command processor, fence parking, thread QoS by role, `os_unfair_lock`,
ARMv8 AES for title decryption, and a lock-free audio ring are part of that picture. Graphics work
that *didn't* pay off is first-class in the ledger (refuted / cancelled / reverted) so the next pass
does not pay for it twice.

### Correctness fixes that are not “suite green”

Structural defects found by audit and instrumentation, not only by `ppc750cl`: out-of-bounds
geometry-shader texture binds; graphic-pack output-shader crash and per-frame OOB pipeline store;
16 KB page unmap holes; partial JIT invalidation; LOD bias never applied; render-pass splits for
framebuffer self-dependency; failed Metal compiles no longer abort the process.

### Platform

Native `GameController.framework`, screensaver inhibition, high-resolution Metal drawables,
symbolicated arm64 crash reports, build-time signing/entitlements. Carbon and SDL-on-main-thread
input are gone.

Live record of every attempt: **[`docs/status/`](docs/status/)**. Hardware notes:
[`docs/hardware/`](docs/hardware/). Test ROMs and strategy: [`testing/`](testing/),
[`docs/testing/`](docs/testing/).

## Building

Apple Silicon Mac (M1 or newer), macOS 26.0+, Xcode 26 command-line tools.

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja

git clone --recursive https://github.com/PowerBeef/TesseraEmu
cd TesseraEmu

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"   # first configure is long without this
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build                                      # ~2 min clean on an M2 after deps

./bin/TesseraEmu_relwithdebinfo -g /path/to/title.wux
```

You need `keys.txt` under `~/Library/Application Support/TesseraEmu/` (Wii U common key and per-title
disc keys from **a console you own**). No keys, game code, or copyrighted assets ship in this repo.

Coming from Cemu? On first run the data directory renames `.../Cemu` → `.../TesseraEmu` on the same
volume when possible (atomic, no copy). If it cannot, your data is left alone and the log says what
to run.

Full flags, bundles, signing: [`BUILD.md`](BUILD.md). Agent/dev brief: [`AGENTS.md`](AGENTS.md).

### CPU suite (no game image)

```sh
# needs official devkitPPC — see testing/toolchain/README.md
cd testing/cpu-tests && make
./run.sh                     | ./report.py -    # recompiler
./run.sh --force-interpreter | ./report.py -    # interpreter
```

Expect `RESULT=PASS failures=0` on both.

## Credit

TesseraEmu is a fork of [Cemu](https://github.com/cemu-project/Cemu) — years of work by many people,
and the only reason this project can exist. The native Metal renderer it builds on was contributed
to Cemu by SamoZ256. `LICENSE.txt` is untouched; the About dialog still credits Exzap and Petergov;
the copyright line is *"Cemu Project and TesseraEmu contributors"* — extended, not replaced.

**Not affiliated with or endorsed by the Cemu project.** The name is separate because MPL-2.0 grants
no trademark rights. Licensed under [MPL-2.0](LICENSE.txt).

> [!NOTE]
> **AI-assisted development.** Cemu asks that contributed code be written and understood by a human,
> for good reasons. **Do not submit these changes upstream as-is.**

Wii and Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo.
