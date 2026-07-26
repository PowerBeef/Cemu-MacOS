# Probes

Small standalone programs used to answer platform questions empirically rather than
from documentation. Each is self-contained; build with plain clang.

| Probe | Question | Result on macOS 26.5.2 / M2 |
|---|---|---|
| `mapjit_region_limit.c` | Does the hardened runtime enforce Apple's documented "only one `MAP_JIT` region per process"? | **No.** 4000 regions succeeded with `--options runtime` + `com.apple.security.cs.allow-jit`. |
| `xbyak_jit_pattern.c` | Does the JIT memory pattern the pinned `xbyak_aarch64` actually uses survive the hardened runtime? | **Yes.** `mmap(PROT_READ\|PROT_WRITE, MAP_JIT)` → write → `mprotect(PROT_READ\|PROT_EXEC)` → execute works signed and unsigned. |
| `aes_armv8_validation.cpp` | Is the ARMv8 crypto-extension AES port correct? | Passes FIPS-197 C.1, an independent key-schedule reference, and 2000 randomized CBC round trips. |
| `mmu_page_align.c` | Does the MemMapper commit/decommit cycle survive 16 KB pages? | **No, before the fix.** `HIGHMEM` (base `0xFFFFF000`) committed fine but decommitted with `EINVAL`, silently leaving guest memory writable across a title unload. `CORE*_LC` was never actually broken -- its base is 16 KB-aligned and the kernel rounds the length up. |

## Building and running

```sh
clang -O1 -o mapjit_region_limit  mapjit_region_limit.c
clang -O1 -o mmu_page_align       mmu_page_align.c
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
