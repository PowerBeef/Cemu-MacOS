# 02 — Memory hierarchy and address map

The Wii U has three physically distinct memory pools with very different characteristics, and a
title's performance depends heavily on which one it puts things in. This is the chapter to read
before reasoning about bandwidth.

## The three pools

| Pool | Size | Technology | Location | Bandwidth | Source |
|---|---|---|---|---|---|
| **MEM0** | 3 MB | 1T-SRAM | Latte die | high, unquantified | `[RE]` |
| **MEM1** | 32 MB | eDRAM | Latte die | ~**140 GB/s** `[EST]` / ~12–15 GB/s `[CONFLICT]` | `[RE]` |
| **MEM2** | 2 GB | DDR3-1600, 4 chips | external | **12.8 GB/s** aggregate (4 × 3.2 GB/s) | `[RE]` |

**MEM1 bandwidth is `[CONFLICT]` and the disagreement is large.** One widely-cited figure is
~140 GB/s (eDRAM on-die); another puts it at 12–15 GB/s, barely above MEM2. No first-party number
exists. Do not build a bandwidth argument on either without measuring — and note that on this port
it is moot, because we have no MEM1/MEM2 distinction at all (see below).

- **MEM0** is OS and vWii territory. In Wii mode it is the Hollywood framebuffer's 1T-SRAM. Titles
  never touch it. **Not mapped by this emulator at all.**
- **MEM1** is the fast pool, intended for render targets and hot working sets. GX2 exposes it
  through `GX2RCreateSurface` with a MEM1 placement hint and `GX2NotifyMemAlloc`/`GX2NotifyMemFree`
  callbacks `[HW Cafe SDK GX2 release notes]`.
- **MEM2** is main memory. Of the 2 GB, **roughly half is reserved for the OS**, leaving ~1 GB to a
  foreground title `[RE]`, which is exactly what we map.

## The guest address map

The full range table, verbatim from `[SRC MMU.cpp:111-126]`. These are the addresses a title sees;
virtual and physical are identity (see below).

| Range | Base | Size | Area ID | Purpose |
|---|---|---|---|---|
| `LOW0` | `0x00010000` | `0x000F0000` | `CODE_LOW0` | code cave (Cemuhook) |
| `TRAMPOLINE_AREA` | `0x00E00000` | `0x00200000` | `CODE_TRAMPOLINE` | HLE trampolines and imports (2 MB) |
| `CODECAVE` | `0x01800000` | `0x00400000` | `CODE_CAVE` | code cave (4 MB) |
| `TEXT_AREA` | `0x02000000` | `0x0C000000` | `CODE_MAIN` | module `.text` (224 MB) |
| `CEMU_AREA` | `0x0E000000` | `0x02000000` | `CEMU_PRIVATE` | emulator-private, 32 MB, mapped early |
| **`MEM2`** | `0x10000000` | `0x40000000` | `MEM2_DATA` | **main memory, 1 GB** |
| `OVERLAY_AREA` | `0xA0000000` | `0x1C000000` | `OVERLAY` | 448 MB, optional (recycled background-app memory) |
| `FGBUCKET` | `0xE0000000` | `0x04000000` | `FGBUCKET` | foreground bucket (64 MB) |
| `TILINGAPERTURE` | `0xE8000000` | `0x02000000` | `TILING_APERATURE` | GPU tiling aperture (32 MB) |
| **`MEM1`** | `0xF4000000` | `0x02000000` | `MEM1` | **32 MB eDRAM** |
| `RPLLOADER_AREA` | `0xF6000000` | `0x02000000` | `RPLLOADER` | RPL loader workspace (kernel space on hardware) |
| `SHARED_AREA` | `0xF8000000` | `0x02000000` | `SHAREDDATA` | shared data, 32 MB, mapped early |
| `CORE0_LC` | `0xFFC00000` | `0x00008000` | `CPU_LC0` | locked cache, core 0 |
| `CORE1_LC` | `0xFFC40000` | `0x00008000` | `CPU_LC1` | locked cache, core 1 |
| `CORE2_LC` | `0xFFC80000` | `0x00008000` | `CPU_LC2` | locked cache, core 2 |
| `PER-CORE` | `0xFFFFC000` | `0x00004000` | `CPU_PER_CORE` | per-core kernel data (current-thread pointer) |

Ranges self-register into `g_mmuRanges` from the `MMURange` constructor `[SRC MMU.cpp:84-87]`, so
adding one is a single declaration.

Two flags exist: `FLAG_MAP_EARLY` (mapped before title load — `CEMU_AREA` and `SHARED_AREA`, because
`SysAllocator` and Cemuhook need them at boot) and `FLAG_OPTIONAL` (`OVERLAY_AREA`, mapped only on
request).

### Notes on individual ranges

**`TEXT_AREA` size disagrees with itself.** The `MMURange` declares `0x0C000000` (192 MB) while the
comment says "0x02000000 to 0x10000000, 224MiB" and `MEMORY_CODEAREA_SIZE` in `MMU.h:157` says
`0x0E000000 // 224MB`. The `MMURange` value is the one that takes effect. `[CONFLICT]`, low impact,
but worth knowing if you are sizing the recompiler jump table — which is derived from the *code
area* size, not this range.

**`TILINGAPERTURE`** is a hardware window that presents linear memory as GPU-tiled (or vice versa)
without a copy — the GPU's detiling unit exposed to the CPU. `OSIsAddressRangeDCValid` hardcodes the
data-cache-valid window as exactly `[0xE8000000, 0xEC000000)` `[SRC coreinit_Memory.cpp:58-68]`. Six
of the eleven `cemu_assert_unimplemented()` sites in GX2 live in `GX2_TilingAperture.cpp` — this is
the least complete part of our GX2 implementation.

**`PER-CORE` was widened for 16 KB pages.** Hardware places it at `0xFFFFF000` with size `0x1000`.
That base is `0x3000` into a 16 KB page, so `mprotect` on Apple Silicon fails. It is declared as
`{0xFFFFC000, 0x4000}` — the containing page — instead `[SRC MMU.cpp:126]`. The comment also records
why it must be *writable* despite us not using it: **Project Zero has a bug where it writes a byte at
`0xFFFFFFFE`.**

**The locked-cache ranges are over-mapped.** Real size is `0x5000`; declared as `0x8000` to reach a
16 KB boundary `[SRC MMU.cpp:122-124]`. Harmless — the next range begins `0x40000` away.

## Virtual ↔ physical is identity

```cpp
uint32 memory_virtualToPhysical(uint32 v) { return v; }   // MMU.cpp:288-291
uint32 memory_physicalToVirtual(uint32 p) { return p; }   // MMU.cpp:293-297
```

Real Espresso has BATs and segment registers and a genuine MMU; Cafe OS sets up a fixed mapping and
titles do not change it. We collapse the whole thing to identity `[SRC]`. This matters in one place:
**GX2 command buffers reference command and resource memory by *physical* address**
(`GX2Command_SubmitCommandBuffer` calls `memory_virtualToPhysical` `[SRC GX2_Command.cpp:230]`), and
because the conversion is identity, those addresses are directly usable as guest pointers. Any future
work that makes the mapping non-trivial has to revisit every PM4 address.

## Fastmem: how a guest address becomes a host address

One contiguous 4 GB reservation, and a bare add:

```cpp
uint8* memory_getPointerFromVirtualOffset(uint32 v) { return memory_base + v; }   // MMU.cpp:311-314
```

`memory_base` is a single `MemMapper::ReserveMemory(nullptr, 0x100000000, P_RW)` at startup
`[SRC MMU.cpp:129-130]` — reserved `PROT_NONE`, with sub-ranges `mprotect`'d in on demand by
`MMURange::mapMem()`.

**There is no bounds check anywhere.** In recompiled code the translation is a single addressing
mode — `AdrExt(MEM_BASE_REG, TEMP_GPR1.WReg, ExtMod::UXTW)` with `x28` holding `memory_base`
`[SRC BackendAArch64.cpp:1017, 1656]`. An unmapped guest access faults to `SIGSEGV`, which the crash
handler reports; it is not serviced.

Two consequences worth stating plainly:

1. **Guest memory access cannot be instrumented cheaply.** A counter on the load/store path would be
   the single most expensive change available in this codebase — it turns one instruction into
   several on the hottest path in the emulator. Memory behaviour has to be inferred at the cache
   layer instead.
2. **There is no page-dirty tracking**, and adding `mprotect`-based tracking would break fastmem
   outright. Anything that needs to know "did the guest write here?" must use an explicit signal —
   which is what `DCFlushRange` → `LatteBufferCache_notifyDCFlush` provides, and what the Latte
   buffer cache's 1 KB page hashing does the hard way.

## Page sizes

| | Size | Source |
|---|---|---|
| Guest (Cafe OS) page | `0x20000` = **128 KB** | `[SRC Const.h:11]` |
| Host page (Apple Silicon) | **16 KB** | `hw.pagesize` |
| Host page (x86 / older ARM) | 4 KB | — |
| Latte buffer-cache page | `0x400` = 1 KB | `[SRC LatteBufferCache.cpp:6]` |

The 16 KB host page is the one that caused real work on this port. `MemMapper::AllocateMemory` /
`FreeMemory` widen outward to page boundaries `[SRC MemMapperUnix.cpp:39-47]`, so a range whose base
or end is only 4 KB-aligned silently makes neighbouring guest memory writable. `memory_init()` now
audits every range against `MemMapper::GetPageSize()` at boot and logs any that would be widened
`[SRC MMU.cpp:141-158]` — a guard against future edits, not a live problem, since all 15 ranges are
declared 16 KB-aligned today.

> Graphic packs still use 4 KB granularity for memory patches, so `MMURange::setEnd` asserts only
> `(end & 0xFFF) == 0` rather than the host page size `[SRC MMU.h:74-82]`. Tightening that assert
> would break working packs.

## MMIO

Two register interfaces, dispatched through four `unordered_map<PAddr, fn>` tables (R32/R16/W32/W16)
`[SRC MMU.h:238-279, MMU.cpp:434-546]`:

- `INTERFACE_0C000000` — the source comment guesses "the old GC register interface?"
- `INTERFACE_0D000000` — "new Wii U stuff?"

Known block addresses `[SRC coreinit_HWInterface.cpp:6-58]`:

| Block | Address |
|---|---|
| VI (userspace) | `0x0C1E0000` |
| **GPU7 / GX2 registers** | `0x0C200000`, length `0x80000`, big-endian; userspace alias `0xFC200000` `[RE]` |
| ACR / VI (kernel) | `0x0D00021C` |
| SI (serial / controllers) | `0x0D006400` |
| AI (audio, probably) | `0x0D046C00` |

Implementation status is thin and deliberately so: `src/Cafe/HW/VI/VI.cpp` has its only registration
commented out, `src/Cafe/HW/AI/AI.h` is an empty file, and `ACR.cpp` logs and discards. Because GX2,
AX and VPAD are all HLE'd, no title reaches the raw registers on a normal path.

## Host memory cost

Not hardware, but it is the reason 8 GB machines are tight. From
`[SRC ../porting/01-foundation-platform-packaging.md]`:

| Consumer | Cost |
|---|---|
| MEM2 | 1 GB committed per title load |
| `TEXT_AREA` | up to 224 MB |
| `OVERLAY_AREA` | 448 MB when requested |
| `FGBUCKET` | 64 MB |
| MEM1 + RPLLOADER + SHARED + CEMU | 128 MB |
| **Recompiler jump table** | **8 MB of host memory per 4 MB of PPC code area** — 8 bytes per guest instruction. A title using all 224 MB of `TEXT_AREA` costs 448 MB of jump table |
| Latte buffer cache | 164 MB `[SRC LatteThread.cpp:128]` |

Realistic floor is **~2 GB RSS before any Metal allocations**. Measured BotW at the shrine: 1367 MB
`[SRC ../../testing/golden/baseline.tsv]`.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| MEM2 (1 GB title portion) | **Modelled** | Mapped at `0x10000000` |
| MEM1 (32 MB) | **Approximated** | Mapped as ordinary memory — no bandwidth or latency distinction from MEM2 |
| MEM0 (3 MB 1T-SRAM) | **Absent** | OS/vWii only; nothing observes it |
| Virtual→physical translation | **Approximated** | Identity. Real BATs/SRs not modelled |
| Address map / range layout | **Modelled** | All 15 ranges, page-aligned for 16 KB hosts |
| Page-level protection | **Approximated** | `mprotect` per range; no per-page guest permissions |
| Guest page size (128 KB) | **Absent** | Only `MEM_PAGE_SIZE` as a constant; not enforced |
| Tiling aperture | **Approximated** | Range mapped; 6 unimplemented sites in `GX2_TilingAperture.cpp` |
| MMIO register blocks | **Absent** | VI empty, AI empty, ACR discards. Unreached because GX2/AX/VPAD are HLE'd |
| Memory bandwidth / contention | **Absent** | No model. MEM1 vs MEM2 costs the same here |
| Dirty-page tracking | **Absent** | Structurally incompatible with fastmem; explicit flush signals used instead |
