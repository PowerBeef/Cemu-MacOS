# Toolchain for building Wii U test ROMs

The test suites in `testing/cpu-tests/` and `testing/graphics-tests/` are **Wii U homebrew**. They
need devkitPPC + wut to build. Neither the emulator nor its normal build needs any of this — only
the tests do.

## Try the normal route first

```sh
curl -fsSLO https://github.com/devkitPro/pacman/releases/download/v6.0.2/devkitpro-pacman-installer.pkg
sudo installer -pkg devkitpro-pacman-installer.pkg -target /
sudo dkp-pacman -Syu --noconfirm
sudo dkp-pacman -S --noconfirm wiiu-dev
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
```

That is the supported path and it is what CI uses (`.github/workflows/cpu_tests.yml`).

## If that fails: `./build-devkitppc.sh`

Builds the whole toolchain from upstream source into `$HOME/.local/devkitpro`. **No root.** ~1 hour.
Safe to re-run.

It exists because both halves of the normal route were closed at once when this suite was written:

1. **The installer needs `sudo`**, which cannot be answered when driving this repo remotely — and an
   agent shell has no TTY, so sudo's credential cache cannot carry across from an interactive one
   either.
2. **`downloads.devkitpro.org` returns 403 via Cloudflare** from some networks. Its *root* 403s while
   `github.com` and `cloudflare.com` return 200, so it is site-specific rather than a general block.
   That host serves **every** source devkitPro's buildscripts fetch — binutils, gcc and newlib
   included, not just their own components — so the block takes out the from-source route too unless
   you work around it.

The way through is that their download loop skips files that already exist
(`if [ ! -f $archive ]`) and honours `BUILD_DKPRO_SRCDIR`, so pre-staging all six archives from
upstream mirrors lets the stock scripts run essentially untouched.

| component | version | fetched from |
|---|---|---|
| binutils | 2.45.1 | ftp.gnu.org |
| gcc | 15.2.0 | ftp.gnu.org |
| newlib | 4.6.0.20260123 | sourceware.org |
| devkitppc-crtls | 1.0.0 | GitHub tag (repacked) |
| devkitppc-rules | 1.2.1 | GitHub tag (repacked) |
| binutils (mn10200) | — | **skipped**, GameCube/Wii DSP, irrelevant to Wii U |

### Two genuine bugs in devkitPro's buildscripts, worked around here

1. **`extract_and_patch binutils $MN_BINUTILS_VER bz2`** passes three arguments to a four-argument
   function (`name ver pkgrel ext`), so `bz2` is read as the package release and the extension ends
   up empty, producing a malformed `tar`. Only affects the mn10200 step, which is skipped.
2. **No `--with-system-zlib`.** The bundled zlib's `zutil.h` does `#define fdopen(fd,mode) NULL`,
   which then breaks the macOS SDK's declaration of `fdopen` in `stdio.h`.

### Three deviations from a stock install — rule these out first if something is odd

1. **`libgloss/libsysbase/dummy.c` is stubbed.** `libgloss/Makefile` lists it but no devkitPro patch
   creates it — it ships inside their repackaged newlib tarball, which is unreachable. 54 sibling
   `libsysbase/*.c` files are present; only this one is absent.
2. **`int32_t`/`uint32_t` are `long`/`unsigned long`** with this newlib rather than
   `int`/`unsigned int`. That forces a one-line signature fix in wut
   (`__syscall_lock_try_acquire_recursive`, to match newlib's `sys/iosupport.h`) and `-Wno-format`
   for wut's build. Both are 32-bit on powerpc-eabi, so this is diagnostic-only, not an ABI
   difference — but it is the most likely explanation for anything strange.
3. **mn10200 skipped**, as above.

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
