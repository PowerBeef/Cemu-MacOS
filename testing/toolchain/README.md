# Toolchain for building Wii U test ROMs

The test suites in `testing/cpu-tests/`, `testing/graphics-tests/` and `testing/rom-tests/` are
**Wii U homebrew**. They need devkitPPC + wut to build. Neither the emulator nor its normal build
needs any of this — only the tests do.

## Install (official, only supported path)

```sh
curl -fsSLO https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg
sudo installer -pkg devkitpro-pacman-installer.pkg -target /
sudo dkp-pacman -Syu --noconfirm
sudo dkp-pacman -S --noconfirm wiiu-dev
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
```

That is the supported path and it is what CI uses (`.github/workflows/cpu_tests.yml`).

`wiiu-dev` installs the compiler, host tools (`elf2rpl`, `wuhbtool`, …) and **wut** together. After
install, `powerpc-eabi-gcc --version` should print a `devkitPPC` line and `make` in any of the test
directories should find `$(DEVKITPRO)/wut/share/wut_rules`.

## CafeGLSL — needed for graphics tests only

Graphics tests compile GLSL to Latte bytecode **at runtime inside the emulator**, which means they
exercise the real Latte→MSL decompiler rather than a synthetic path. That needs
[CafeGLSL](https://github.com/Exzap/CafeGLSL) (by Cemu's own author):

```sh
mkdir -p ~/Library/Application\ Support/TesseraEmu/cafeLibs
curl -fsSL -o ~/Library/Application\ Support/TesseraEmu/cafeLibs/glslcompiler.rpl \
  https://github.com/Exzap/CafeGLSL/releases/download/v0.2.0/glslcompiler.rpl
```

`LoadSharedLibrariesEnabled()` defaults to `true`, so **no config change is needed**. Limits worth
knowing: separable shaders only, explicit binding locations, no geometry/compute/tessellation.

## Two traps that cost real time

- **A homebrew sample reading zero frames is far more likely to be its own asset path than an
  emulator defect.** `wut`'s `gx2_triangle` reads its shader from the *SD card*
  (`WHBGetSdCardMountPath()` → `<userdata>/sdcard/`), **not** from the `.wuhb` bundle's
  `/vol/content`. Bundling it produced a clean boot, zero frames, and no error — because:
- **Many samples log over UDP** (`WHBLogUdpInit`), so `--forward-console-logging` shows nothing.
  `wut-tools` ships `udplogserver`, which receives it. Our own ROMs use `WHBLogCafeInit` (OSReport)
  precisely so the standard flag works.
