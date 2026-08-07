<div align="center">

<img src="docs/assets/tesseraemu-icon.png" width="120" alt="TesseraEmu">

# TesseraEmu

### Wii U emulator · hard-forked for Apple Silicon

`arm64` &nbsp;·&nbsp; `Metal` &nbsp;·&nbsp; `macOS 26+`

<br>

| | | |
|:---:|:---:|:---:|
| **ppc750cl full suite** | **MK8 locked** | **BotW Korok** |
| `0` failures · both arms | `60 fps` @ ~`104%` core | `20.04 fps` · `30.06` w/ sync off |
| *Espresso silicon suite* | *was ~`184%`* | *config tradeoff, not GPU* |

<br>

[Status ledger](docs/status/) · [How suite went green](docs/testing/fpscr-suite-green.md) · [Build](BUILD.md) · [Porting](docs/porting/)

</div>

---

> [!IMPORTANT]
> **No numbered releases yet.** Thin play matrix, no public compatibility list. For day-to-day multi-platform play, use [Cemu](https://github.com/cemu-project/Cemu). This fork is for a measured arm64/Metal stack — not a second portable frontend.

---

## What it is

A **deliberate hard fork** of [Cemu](https://github.com/cemu-project/Cemu): one CPU backend, one graphics API, one OS floor. Portability shims deleted so the code can assume the machine.

| Constraint | What shipped |
|:---|:---|
| **arm64 only** | Single JIT (`BackendAArch64`); x86 IML / `BackendX64` gone |
| **Metal only** | Latte → MSL at runtime · no GL · no Vulkan · no MoltenVK |
| **macOS 26.0+** | 16 KB pages, modern APIs — not runtime-probed |

A *tessera* is one mosaic tile. The mark is a **T** with one tile still resolving: accuracy and performance land one measured piece at a time.

---

## Verified

All figures live in the [status ledger](docs/status/). Do not invent new ones in prose.

### Guest CPU — Espresso conformance

[Andrew Church's `ppc750cl.s`](https://achurch.org/cpu-tests/ppc750cl.s) (public domain, **validated on real Espresso**) runs as Wii U homebrew — no game image, keys, or SDK.

| Arm | Full FPSCR | Values only | Unique |
|:---|---:|---:|---:|
| Recompiler | **0** | **0** | **0** |
| Interpreter | **0** | **0** | **0** |

Results **and** FPSCR (FPRF, FI, stickies), including all `ps_*` / `psq_*`. Path: **1,030** → **928** (values closed) → **0**. Write-up: [fpscr-suite-green.md](docs/testing/fpscr-suite-green.md).

### Performance

| Scene | Result | Note |
|:---|:---|:---|
| Mario Kart 8 | **60 fps** locked · ~**104%** of one core | was ~**184%** (idle-spin work) |
| BotW Korok Forest | **20.04 fps** default | **30.06 fps** if `GX2DrawdoneSync` off |
| Korok GPU (active samples) | **45.6%** ALU-limited | tex 22.8% · mem path ~12.8% |

Refuted graphics experiments stay in the ledger so they are not re-run for free.

### Also landed

| Area | Highlights |
|:---|:---|
| **Correctness** | OOB texture binds · pack output-shader crash · 16 KB unmap · partial JIT invalidation · LOD bias · self-dep pass split · failed MSL no longer aborts |
| **Platform** | `GameController` · HR Metal drawable · symbolicated arm64 crashes · build signing · Carbon / main-thread SDL input removed |
| **Host** | QoS by role · `os_unfair_lock` · ARMv8 AES decrypt · lock-free audio ring |

---

## Build

**Needs:** Apple Silicon · macOS 26.0+ · Xcode 26 CLT

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja
git clone --recursive https://github.com/PowerBeef/TesseraEmu && cd TesseraEmu

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"   # first configure is long
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build                                      # ~2 min clean on M2 after deps

./bin/TesseraEmu_relwithdebinfo -g /path/to/title.wux
```

| | |
|:---|:---|
| **Keys** | `~/Library/Application Support/TesseraEmu/keys.txt` — from a console **you own** |
| **From Cemu** | Data dir renames `…/Cemu` → `…/TesseraEmu` when possible (same volume) |
| **Flags / bundle** | [BUILD.md](BUILD.md) · agent brief [AGENTS.md](AGENTS.md) |

<details>
<summary><strong>CPU suite</strong> (no game image — expect <code>PASS failures=0</code>)</summary>

<br>

```sh
# official devkitPPC — testing/toolchain/README.md
cd testing/cpu-tests && make
./run.sh                     | ./report.py -   # recompiler
./run.sh --force-interpreter | ./report.py -   # interpreter
```

</details>

---

## Docs map

| | |
|:---|:---|
| [docs/status/](docs/status/) | Live ledger — landed / refuted / measured |
| [docs/testing/](docs/testing/) | Test strategy · [FPSCR write-up](docs/testing/fpscr-suite-green.md) |
| [docs/porting/](docs/porting/) | Staged plan · CPU/JIT · Metal |
| [docs/hardware/](docs/hardware/) | Guest hardware notes |
| [testing/](testing/) | CPU / graphics / ROM suites |

---

## Credit

Fork of [Cemu](https://github.com/cemu-project/Cemu). Native Metal path originated with SamoZ256's Cemu work. Licence [MPL-2.0](LICENSE.txt) unchanged; About still credits Exzap and Petergov; copyright is *Cemu Project and TesseraEmu contributors*.

**Not affiliated with or endorsed by the Cemu project** (no MPL trademark grant).

> [!NOTE]
> **AI-assisted development.** Do **not** submit these changes upstream to Cemu as-is.

Wii / Wii U are trademarks of Nintendo. TesseraEmu is not affiliated with Nintendo.
