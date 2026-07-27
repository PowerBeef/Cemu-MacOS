# Build Instructions

TesseraEmu builds only for **Apple Silicon on macOS 26 or later**. There is no Windows, Linux or
Intel build: `src/Common/precompiled.h` `#error`s on any target other than arm64, the x86-64
recompiler backend is deleted, and the OpenGL and Vulkan/MoltenVK renderers are gone. Upstream
Cemu's multi-platform build instructions do not apply here — in particular, **MoltenVK is not a
dependency and must not be installed for this**.

## Requirements

- Apple Silicon Mac (M1 or newer)
- macOS 26.0 or later
- Xcode 26 command line tools (`xcode-select --install`)

## Dependencies

Install [Homebrew](https://brew.sh) if you don't have it, then:

```sh
brew install pkgconf nasm automake autoconf libtool cmake ninja
```

Everything else — wxWidgets, SDL3, boost, libpng, zlib and the rest — is built from source by
vcpkg during the first CMake configure. Nothing is taken from Homebrew at link time.

## Building

```sh
git clone --recursive https://github.com/PowerBeef/TesseraEmu
cd TesseraEmu

export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"    # first build is ~25 min without this
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build
```

This produces `bin/TesseraEmu_relwithdebinfo`. The first configure builds every dependency from
source and takes a while; incremental builds after that are around two minutes on an M2.

If you cloned without `--recursive`, fetch just the submodules that are still used:

```sh
git submodule update --init --depth 1 \
  dependencies/{vcpkg,ZArchive,cubeb,imgui,metal-cpp,xbyak_aarch64}
```

### Build types

| `CMAKE_BUILD_TYPE` | Output | Notes |
|---|---|---|
| `RelWithDebInfo` | `bin/TesseraEmu_relwithdebinfo` | Day-to-day. LTO is deliberately **off**, which keeps relinks fast and keeps `.o` files real Mach-O so `llvm-objdump` can inspect generated code. |
| `Release` | `bin/TesseraEmu_release` | ThinLTO on; object files become LLVM bitcode. |
| `Debug` | `bin/TesseraEmu_debug` | |

### App bundle

`-DMACOS_BUNDLE=ON` produces `bin/TesseraEmu_<config>.app` instead of a raw executable, and signs
it as part of the build: hardened runtime, ad-hoc signature, and the
`com.apple.security.cs.allow-jit` entitlement from `dist/macos/TesseraEmu.entitlements`
(`TesseraEmu.debug.entitlements`, which adds `get-task-allow` so `lldb` can attach, for Debug and
RelWithDebInfo).

**That entitlement is not optional.** The PowerPC recompiler emits code at runtime through
`mmap(..., MAP_JIT)`, which the hardened runtime refuses without it. No Apple Developer Program
membership is needed for local development — ad-hoc signing is sufficient. Set
`CEMU_CODESIGN_IDENTITY` to a Developer ID for a distributable build, and
`CEMU_CODESIGN_TIMESTAMP=--timestamp` to have it timestamped.

Use `MACOS_BUNDLE=OFF` for day-to-day work; bundle builds are for signing and entitlement testing.

### Troubleshooting

- **CMake can't find ninja**: append `-DCMAKE_MAKE_PROGRAM=/opt/homebrew/bin/ninja`.
- **Confirm the deployment target actually took**:
  `otool -l bin/TesseraEmu_relwithdebinfo | grep -A4 LC_BUILD_VERSION` → `minos 26.0`.

## CMake configure flags

Example: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_DISCORD_RPC=OFF`

| Flag | Description | Default |
|---|---|---|
| `MACOS_BUNDLE` | Build an application bundle instead of a raw executable, and sign it | ON |
| `ENABLE_VCPKG` | Use vcpkg to obtain dependencies | ON |
| `ALLOW_PORTABLE` | Allow a `portable/` directory beside the executable to hold config and data | ON |
| `CEMU_LTO` | ThinLTO for Release builds | ON where supported |
| `CEMU_CXX_FLAGS` | Extra flags passed to the compiler, e.g. `-march=native` | "" |
| `ENABLE_CUBEB` | cubeb audio backend | ON |
| `ENABLE_SDL` | SDL controller API | ON |
| `ENABLE_HIDAPI` | HIDAPI, used for the Wiimote controller API | ON |
| `ENABLE_LIBUSB` | libusb | ON |
| `ENABLE_DISCORD_RPC` | Discord Rich Presence | ON |
| `ENABLE_WXWIDGETS` | wxWidgets UI | ON (currently required) |
| `CEMU_CODESIGN_IDENTITY` | codesign identity; `-` means ad-hoc | `-` |
| `CEMU_CODESIGN_TIMESTAMP` | codesign timestamp flag | `--timestamp=none` |

There are no `ENABLE_OPENGL` or `ENABLE_VULKAN` flags — Metal is the only renderer.

## Keeping up to date

```sh
git pull --recurse-submodules
```

## Running

See [README.md](/README.md) for `keys.txt` setup, and [CLAUDE.md](/CLAUDE.md) for architecture
notes, profiling commands and the verification workflow.
