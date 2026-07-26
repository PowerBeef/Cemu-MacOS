# Probes

Small standalone programs used to answer platform questions empirically rather than
from documentation. Each is self-contained; build with plain clang.

| Probe | Question | Result on macOS 26.5.2 / M2 |
|---|---|---|
| `mapjit_region_limit.c` | Does the hardened runtime enforce Apple's documented "only one `MAP_JIT` region per process"? | **No.** 4000 regions succeeded with `--options runtime` + `com.apple.security.cs.allow-jit`. |
| `xbyak_jit_pattern.c` | Does the JIT memory pattern the pinned `xbyak_aarch64` actually uses survive the hardened runtime? | **Yes.** `mmap(PROT_READ\|PROT_WRITE, MAP_JIT)` → write → `mprotect(PROT_READ\|PROT_EXEC)` → execute works signed and unsigned. |
| `aes_armv8_validation.cpp` | Is the ARMv8 crypto-extension AES port correct? | Passes FIPS-197 C.1, an independent key-schedule reference, and 2000 randomized CBC round trips. |
| `sigaltstack_overflow.cpp` | Can a stack-overflow SIGSEGV be reported without an alternate signal stack? | **No.** Without `sigaltstack`+`SA_ONSTACK` the process dies with exit 139 and the handler never runs -- no crash log, no output. With it, the handler runs and captures 32 frames. |
| `sdl_background_thread_gamepad.cpp` | Can SDL's gamepad subsystem be initialised and polled from a non-main thread on macOS? | **Yes.** `SDL_INIT_GAMEPAD \| SDL_INIT_HAPTIC` (no `SDL_INIT_VIDEO`) works off the main thread and enumerates connected pads. Only the *video* subsystem needs the main thread, which is why removing the SDL-based screensaver code unblocked moving the event pump to a dedicated thread. |
| `gamecontroller_live_input.mm` | Does the GameController provider's bridge read a real pad correctly? | **Yes.** Enumerates the device, and reports face buttons, d-pad, shoulders, analog triggers (intermediate values, not just on/off) and both sticks across the full range. Note `GCController.controllers` is **empty until the run loop spins** -- the framework delivers devices by notification, not synchronously. |
| `mmu_page_align.c` | Does the MemMapper commit/decommit cycle survive 16 KB pages? | **No, before the fix.** `HIGHMEM` (base `0xFFFFF000`) committed fine but decommitted with `EINVAL`, silently leaving guest memory writable across a title unload. `CORE*_LC` was never actually broken -- its base is 16 KB-aligned and the kernel rounds the length up. |

## Building and running

```sh
clang -O1 -o mapjit_region_limit  mapjit_region_limit.c
clang -O1 -o mmu_page_align       mmu_page_align.c
clang++ -std=c++20 -I ../../src -o gamecontroller_live_input gamecontroller_live_input.mm \
    ../../src/input/api/GameController/GCController_mac.mm \
    -framework Foundation -framework GameController
clang++ -O0 -std=c++20 -o sigaltstack_overflow sigaltstack_overflow.cpp   # run with and without the 'altstack' argument
clang -O1 -o xbyak_jit_pattern    xbyak_jit_pattern.c
clang++ -std=c++20 -O2 -march=armv8-a+crypto -o aes_validation aes_armv8_validation.cpp

# to test under the hardened runtime (the shipping configuration):
codesign --force --options runtime --entitlements jit.entitlements --sign - <binary>
```

## Why these matter

The porting plan assumed the one-region `MAP_JIT` limit was enforced and treated a
first-party JIT arena as a hard blocker for signing. These probes show it is not
enforced, and that the existing allocator works signed. The arena is still worth
building -- recompiled code is currently never freed -- but it is an optimization,
not a gate on distribution.

A second caution: an early reading of xbyak concluded it performed no I-cache
maintenance, based on a grep that only covered `xbyak_aarch64/*.h`. It does --
`CodeGenerator::clearCache` lives in `src/xbyak_aarch64_impl.h` and calls
`sys_icache_invalidate` on Apple. Grep the whole submodule, not just its public
headers.

A caution recorded from writing these: the first version of `mapjit_region_limit.c`
allocated `PROT_READ|PROT_WRITE|PROT_EXEC` and toggled `pthread_jit_write_protect_np`.
That pattern SIGBUSes on write, but it is *not* what xbyak does -- xbyak maps RW-only
and never holds a region writable and executable at once. Test the pattern the code
actually uses, not the one the documentation describes.
