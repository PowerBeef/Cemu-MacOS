# 01 — Espresso: the guest CPU

Espresso is a tri-core PowerPC processor built by IBM on 45 nm SOI. It is a die-shrunk,
triple-core, higher-clocked **Broadway** — which is itself a higher-clocked **Gekko** — which is a
PowerPC 750CXe with a custom FPU and a handful of graphics-oriented additions bolted on `[RE]`.
That lineage is the single most useful fact about it: almost every Espresso oddity is a GameCube-era
design decision that survived two hardware generations.

```
PowerPC 750CXe  →  Gekko (GameCube)  →  Broadway (Wii)  →  Espresso (Wii U)
   base G3          + paired singles     + clock/cache      + 3 cores, + L2,
                    + GQRs, + locked        bump               + secure boot ROM,
                      cache, + WGP                             + MERSI coherency
```

## Cores and clocks

| | Value | Source |
|---|---|---|
| Cores | 3 (CPU0, CPU1, CPU2), 1 thread each | `[SRC Const.h:5]` |
| Core clock | **1,243.125 MHz** | `[SRC Const.h:7]` |
| Bus clock | **248.625 MHz** = core / 5 | `[SRC Const.h:8]` |
| Timebase clock | **62.15625 MHz** = bus / 4 = **core / 20** | `[SRC Const.h:9]` |
| Process | 45 nm SOI, eDRAM L2 | `[RE]` |

```cpp
// src/Cafe/HW/Espresso/Const.h
constexpr inline uint64 CORE_CLOCK  = 1243125000;
constexpr inline uint64 BUS_CLOCK   = 248625000;
constexpr inline uint64 TIMER_CLOCK = BUS_CLOCK / 4;
```

The core/20 relationship is exact and is worth committing to memory — it is how `mftb` is derived
(`coreinit_GetMFTB()` is literally `PPCInterpreter_getMainCoreCycleCounter() / 20ULL`
`[SRC coreinit_Time.cpp:6-11]`), and the same ratio appears as
`ESPRESSO_CORE_CLOCK_TO_TIMER_CLOCK(cc) ((cc)/20ULL)` `[SRC PPCState.h:174]`.

> **Duplicate constants.** `PPCState.h:170-174` defines `ESPRESSO_CORE_CLOCK` / `ESPRESSO_BUS_CLOCK`
> / `ESPRESSO_TIMER_CLOCK` with the same values as `Const.h`. Both sets are live and widely used.
> `Const.h` is canonical; the `PPCState.h` macros are legacy.

### The timebase accuracy ceiling

This is the most consequential timing fact on this port, and it is a hard limit rather than a bug:

| Clock | Frequency | Period |
|---|---|---|
| Guest timebase (`mftb`) | 62.15625 MHz | **16.09 ns** |
| Host `cntvct_el0` (Apple Silicon) | 24 MHz | **41.67 ns** |

**The host clock is 2.6× coarser than the clock we are emulating.** Every guest timebase read is a
scaled host counter read, so guest time advances in steps of ~2.6 guest ticks. There is no finer
clock available on the platform — `mach_absolute_time()` and `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)`
are both `cntvct_el0` × `mach_timebase_info` plus call overhead `[SRC ../porting/02-cpu-jit-memory.md §4.1]`.

The conversion is an exact rational, which is why the fork reads the counter directly rather than
calibrating: `CORE_CLOCK / cntfrq = 1243125000 / 24000000 = 3315 / 64` `[SRC PPCTimer.cpp:31-32]`.

**Consequence for accuracy work:** a title that measures a sub-42 ns interval cannot get a correct
answer from us, and any emulator-side instrumentation that tries to time sub-microsecond events with
this counter is quantised to ±20% or worse. This is why `04`'s advice on measuring GPU stages is
"count events, don't time them".

## Cache hierarchy

| Level | Size | Organisation | Source |
|---|---|---|---|
| L1 instruction | 32 KB | 8-way set-associative | `[RE]` |
| L1 data | 32 KB | 8-way, with a 16 KB lockable region | `[RE]` |
| L2 core 0 | 512 KB | 2-way, eDRAM | `[RE]` `[SRC coreinit_SystemInfo.cpp:19]` |
| L2 core 1 | **2 MB** | 2-way, eDRAM | `[RE]` — but see the bug below |
| L2 core 2 | 512 KB | 2-way, eDRAM | `[RE]` `[SRC coreinit_SystemInfo.cpp:21]` |
| Cache line | **32 bytes** | | `[SRC coreinit_Memory.cpp:11-56]` |

The asymmetry is real and deliberate: **core 1 gets 4× the L2 of the others**, which is why Cafe OS
runs its kernel there `[RE]`. A title that pins a heavy thread to core 1 is exploiting hardware, not
being arbitrary.

> **`[SRC]` bug — `coreinit_SystemInfo.cpp:21`.**
> ```cpp
> g_system_info->l2cacheSize[1] = 2*1024*1924; // 2MB
> ```
> `1924` is a typo for `1024`. The guest is told core 1 has **3,940,352 bytes** (3.76 MB) instead of
> 2,097,152. Any title that sizes a working set off `OSGetSystemInfo()` gets a wrong answer.
> Tracked in [`09-accuracy-gap-register.md`](09-accuracy-gap-register.md).

### The 32-byte cache line

There is no named constant for it — it is open-coded as `~31` masks throughout the cache ops:

```cpp
// src/Cafe/OS/libs/coreinit/coreinit_Memory.cpp
void DCFlushRange(MPTR addr, uint32 size) {
    MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
    addr &= ~0x1F;
    LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);
}
```

`DCZeroRange` makes the granularity explicit: it computes `blocks = (offset + size + 31) / 32` and
`memset`s `blocks * 32` bytes `[SRC coreinit_Memory.cpp:46-56]`. **A guest `DCZeroRange` therefore
zeroes more than it was asked to** whenever the range is not line-aligned — that is correct hardware
behaviour, not an emulator artifact, and titles rely on it.

### Cache coherency and why titles flush manually

Espresso uses **MERSI** coherency across the three cores `[RE]`. But the GPU is *not* in the
coherency domain — the CPU's data cache is not snooped by Latte. Every buffer a title hands to the
GPU must be explicitly written back with `DCFlushRange`/`DCStoreRange`, and this is why
`GX2Invalidate` exists (see [`04-gx2-command-model.md`](04-gx2-command-model.md)).

The emulator has no CPU cache model at all, so guest writes are immediately visible everywhere. That
is *more* permissive than hardware: **a title with a missing `DCFlushRange` works here and fails on
console.** We inherit the correct behaviour by accident. What we must get right is the opposite
direction — using the flush notifications to invalidate our GPU-side buffer cache:

| Guest call | Notifies `LatteBufferCache` | `[SRC]` |
|---|---|---|
| `DCFlushRange` | yes | `coreinit_Memory.cpp:19-24` |
| `DCFlushRangeNoSync` | yes | `:26-31` |
| `DCStoreRangeNoSync` | yes | `:40-45` |
| `DCZeroRange` | yes | `:46-56` |
| `DCStoreRange` | **no — commented out** | `:33-38` |
| `DCInvalidateRange` | **no — commented out** | `:11-17` |

`DCInvalidateRange` omitting the notify is defensible: it discards the CPU's copy without writeback,
so there is nothing new for the GPU to see. **`DCStoreRange` is not defensible.** Store writes dirty
lines back to memory — exactly the case the GPU cache needs to hear about — and its own `NoSync`
variant does notify. That asymmetry is an accuracy gap (chapter 09).

### The locked cache

16 KB of L1D per core can be locked and addressed directly as scratchpad — a GameCube feature that
survived intact. It is mapped, not allocated:

```
core 0:  0xFFC00000  0x4000      // 16 KB
core 1:  0xFFC40000  0x4000
core 2:  0xFFC80000  0x4000
```
`[SRC coreinit_LockedCache.cpp:5-14]`, allocation granularity `LC_LOCKED_CACHE_GRANULARITY = 0x200`
(512 B), size `LC_LOCKED_CACHE_SIZE = 0x4000`.

In the MMU table these appear as `mmuRange_CORE0/1/2_LC` with size `0x8000` rather than `0x5000` —
rounded up so the base and end land on Apple Silicon's 16 KB page boundaries
`[SRC MMU.cpp:122-124]`. The next range starts 0x40000 away, so the over-map is harmless. See
[`02-memory-hierarchy.md`](02-memory-hierarchy.md).

### The write-gather pipe

A store-combining path from the CPU straight to the GPU FIFO, inherited from Gekko, where it existed
so the CPU could stream display lists without polluting the cache `[HW]`. On Wii U it is what GX2's
command-buffer writer uses (see [`04-gx2-command-model.md`](04-gx2-command-model.md)).

**It has a documented hardware bug.** The Cafe SDK release notes record that the CPU write-gatherer
corrupts command buffers under rare circumstances, causing GPU hangs; SDK 2.08.03 shipped a
workaround that "result[s] in some increase in CPU costs for most GX2 APIs", refined to a lower-cost
form in 2.08.04 `[HW Cafe SDK GX2 release notes]`. We do not model the write-gather pipe as
hardware — GX2 is HLE'd — so neither the bug nor its workaround is reproduced. Titles built against
those SDK versions carry the workaround's *cost* in their own code, which we do execute.

## Floating point

### Paired singles

The FPU is 64-bit but every register is addressable as **two packed 32-bit floats**, `ps0` and
`ps1`, with a full complement of paired arithmetic instructions (`ps_add`, `ps_mul`, `ps_madd`,
`ps_sum0/1`, `ps_merge*`, …). This is Gekko's headline addition and it is used heavily by Wii U
titles for vertex and physics maths.

```cpp
// src/Cafe/HW/Espresso/PPCState.h:14-31 — the register file is a union
union FPR_t {
    double fpr;
    struct { double fp0; double fp1; };   // paired-single view
    struct { uint64 guint; };
    struct { uint64 fp0int; uint64 fp1int; };
};
```

`FPR_t` is 16 contiguous bytes, which lets the AArch64 backend load and store a whole paired register
with a single `ldr q` / `str q` `[SRC ../porting/02-cpu-jit-memory.md §Verification]`.

> **Accuracy gap — `frC` rounding.** Real Espresso rounds the `frC` operand of a paired-single
> multiply to **25 bits** before multiplying, a quirk inherited from Gekko. We do not:
> `// todo - round fprC to 25bit accuracy` appears at `[SRC PPCRecompilerImlGenFPU.cpp:1025, 1065, 1171]`.
> Results differ in the low bits. Deliberately not implemented — the cost is high and no title is
> known to depend on it — but it is recorded in chapter 09 because "no known title" is not "no title".

> **Accuracy gap — double rounding in `fmadd`.** PPC `fmadd` is a *fused* multiply-add: one
> rounding, at the end. Our lowering computes `round(round(a·c) + b)` — two roundings — which is
> architecturally wrong, not merely imprecise. Detail and the `fmsub`↔`fnmsub` mnemonic crossover
> trap are in `[SRC ../porting/02-cpu-jit-memory.md §3.2]`.

### Graphics Quantization Registers (GQRs)

Eight registers, `UGQR0`–`UGQR7` `[SRC PPCState.h:63]`, each holding a load type, a store type, and
scale exponents. They parameterise the quantised load/store instructions `psq_l`, `psq_lu`,
`psq_st`, `psq_stu` (primary opcodes 56, 57, 60, 61 `[SRC EspressoISA.h:80-81]`) and their indexed
forms `psq_lx`/`psq_stx`.

The type field selects the in-memory format, and conversion to/from `float` is free:

```cpp
// src/Cafe/HW/Espresso/EspressoISA.h:13-19
enum class PSQ_LOAD_TYPE {
    TYPE_F32 = 0,   TYPE_UNUSED1 = 1, TYPE_UNUSED2 = 2, TYPE_UNUSED3 = 3,
    TYPE_U8  = 4,   TYPE_U16     = 5, TYPE_S8      = 6, TYPE_S16     = 7,
};
```

This is a hardware type-conversion unit in the load/store path. It is why Wii U vertex data is so
often 8- or 16-bit: dequantisation costs nothing. `psq_l` with `TYPE_S16` and a scale loads two
16-bit fixed-point values and delivers two floats in one instruction — which is why
[`../porting/02-cpu-jit-memory.md`](../porting/02-cpu-jit-memory.md) ranks `psq_l`/`psq_st` lowering
via `fcvtl`/`fcvtn` as a real codegen win (~10 instructions → 4).

## Atomics: `lwarx` / `stwcx.`

Load-and-reserve / store-conditional, the PowerPC atomic primitive. State is a single
address/value pair per core:

```cpp
uint32 reservedMemAddr;
uint32 reservedMemValue;      // src/Cafe/HW/Espresso/PPCState.h:66-67
```

with `PPC_LWARX_RESERVATION_MAX = 4` `[SRC PPCState.h:12]`. The reservation is cleared on every
timeslice boundary `[SRC coreinit_Thread.cpp:1383-1384]`.

Copetti notes that Espresso "breaks PowerPC multi-processing instructions (`lwarx`/`stwcx`)
requiring manual cache flushes" `[RE]` — i.e. the reservation does not interact with the cache
hierarchy the way stock PowerPC specifies, which is part of why Cafe OS synchronisation primitives
flush explicitly rather than relying on the atomics alone.

## Registers we do not model

| Register | Real behaviour | Ours | `[SRC]` |
|---|---|---|---|
| `PVR` | Processor version | returns `0x70010101`, comment says **"guessed"** | `PPCInterpreterSPR.hpp:58-61` |
| `HID0`,`HID1`,`HID2`,`HID4`,`HID5` | Cache/clock/feature control | all return `0` | `:99, :110, :116, :128, :140` |
| `DEC` (decrementer) | Counts down at timebase rate, raises exception at 0 | read path opens with `assert_dbg()` — **not expected to be reached** | `:813-822` |
| `L2CR`, `CAR`, `BCR`, BATs, SRs | MMU/cache config | SPR numbers defined, values inert | `:4-56` |

Guest-visible SPR state we actually keep is small: `LR`, `CTR`, `XER`, `UPIR`, `UGQR[8]`
`[SRC PPCState.h:57-64]`. `UPIR` is the core index — `PPCInterpreter_getCurrentCoreIndex()` is
literally `spr.UPIR` `[SRC PPCInterpreterMain.cpp:91-95]`, and it is **the correct key for per-core
attribution**; the host-side `t_assignedCoreIndex` disagrees with it in singlecore mode.

The decrementer being inert is the notable absence. Exception vectors are defined
(`CPU_EXCEPTION_DECREMENTER 0x00000900 // todo: validate` `[SRC PPCState.h:181]`) but nothing raises
them. Cafe OS drives scheduling from `OSAlarm` and the timebase rather than DEC interrupts, so no
title has needed it — but a title that programmed DEC directly would silently never be interrupted.

## How this maps onto the host

Not hardware, but you cannot reason about accuracy here without it.

- **PPC → IML → AArch64 recompiler.** Guest state lives in `PPCInterpreter_t` (1176 bytes), pinned
  in `x29` for the duration of a JIT invocation, with `x27` = jump-table base and `x28` = guest
  memory base `[SRC BackendAArch64.cpp:22-24]`.
- **13 `static_assert`s pin field offsets** into AArch64's scaled-immediate addressing range
  `[SRC BackendAArch64.cpp:365-378]`. The tightest is `temporaryFPR` at offset 752, which must stay
  16-byte aligned because it is accessed with `ldr q`. Fields appended after `rspTemp` (offset 1168)
  are free; anything *inserted* before 752 must be a multiple of 16 bytes or the build fails. This is
  by design — it turns a silent codegen corruption into a compile error.
- **Guest threads are `ucontext` fibers**, not host threads, with `PPCInterpreter_t` embedded by
  value in `OSHostThread` `[SRC coreinit_Thread.cpp:62-76]`. See
  [`05-cafeos-scheduling.md`](05-cafeos-scheduling.md).
- **Timeslice length** is `ppcThreadQuantum = 45000` guest instructions `[SRC PPCScheduler.cpp:10]`,
  plus a deliberate jitter — see chapter 05, and do not remove it.
- **No cache model.** No L1, no L2, no line-fill costs, no coherency traffic. Cycle counting is
  instruction-count-based, not cycle-accurate. Nothing observable depends on it today.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| Integer + FP instruction semantics | **Modelled** | Interpreter and JIT |
| Paired-single arithmetic | **Modelled** | |
| GQR quantised load/store | **Modelled** | Interpreter path; JIT lowering is naive (perf, not accuracy) |
| `fmadd` fusion | **Approximated** | Double-rounds. Architecturally wrong → ch. 09 |
| `frC` 25-bit rounding | **Absent** | Deliberate → ch. 09 |
| `lwarx`/`stwcx.` | **Approximated** | Single reservation, cleared per timeslice |
| Timebase / `mftb` | **Approximated** | Exact ratio, but host counter is 2.6× coarser |
| Decrementer | **Absent** | Read path asserts; nothing raises the exception |
| `PVR`, `HID0-5` | **Absent** | PVR is a guess, HIDs return 0 |
| L1 / L2 caches | **Absent** | No cache model at all |
| Cache line granularity | **Modelled** | 32-byte rounding in `DC*Range` is correct |
| `DCStoreRange` → GPU cache notify | **Absent** | Commented out; `NoSync` variant does it → ch. 09 |
| Locked cache | **Modelled** | Mapped memory + allocator, 512 B granularity |
| Write-gather pipe | **Absent** | GX2 is HLE'd; pipe is never modelled as hardware |
| MERSI coherency | **Absent** | Shared memory is trivially coherent for us |
| L2 size reported to guest | **Approximated** | Core 1 value is wrong (typo) → ch. 09 |
| Per-core cycle timing | **Approximated** | Instruction-count quantum, not cycles |
