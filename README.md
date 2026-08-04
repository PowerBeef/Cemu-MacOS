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
> **This is early.** No releases yet, two games verified on one machine, no compatibility claims.
> If you want to play something today, [Cemu](https://github.com/cemu-project/Cemu) is the mature,
> well-supported project and you should use it. Come back here when this one has earned it.

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

## How it is built

Every change here is measured before and after, and the results are published whether or not they
flattered the idea. Roughly half the optimisations tried did not work, and those are on the record
too, because knowing what fails is the part that saves the next person time.

- [`docs/status/`](/docs/status/) is the live record: every item attempted, what it measured, and
  which ones were refuted, cancelled or reverted.
- [`docs/hardware/`](/docs/hardware/) is a reference on the Wii U's actual silicon, with every claim
  tagged by where it came from.
- [`testing/`](/testing/) holds the test ROMs, which are homebrew and need no game image.

## Credit

TesseraEmu is a fork of [Cemu](https://github.com/cemu-project/Cemu), which is the work of many
people over many years and is the reason this project could exist at all. The Metal renderer it
builds on was contributed to Cemu by SamoZ256. `LICENSE.txt` is untouched, the About dialog still
credits Exzap and Petergov, and the copyright line reads *"Cemu Project and TesseraEmu
contributors"*, extended rather than replaced.

This project is **not affiliated with or endorsed by the Cemu project**, and carries its own name
because MPL-2.0 grants no trademark rights. Licensed under [MPL-2.0](/LICENSE.txt).

> [!NOTE]
> **Changes here were written with AI assistance (Claude).** Cemu asks that contributed code be
> written and understood by a human, for good reasons. **Do not submit these changes upstream as-is.**

Wii and Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo.
