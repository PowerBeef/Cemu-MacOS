# 05 — Cafe OS threading and scheduling

The chapter with the most accuracy leverage per page, and the least written down anywhere else.

Espresso runs a real operating system — a departure from the Wii's bare-metal model. Cafe OS is a
microkernel-ish layer with a bootloader and kernel running in supervisor mode on **core 1** (which
has 4× the L2 of the other two — see chapter 01) `[RE]`. Titles are RPX executables with dynamically
linked RPL libraries, and only two processes exist at once: one foreground (≤ 960 MB) and one
background `[RE]`.

## The headline fact: scheduling is cooperative

**Cafe OS threads are not preempted by the scheduler on quantum expiry the way a desktop OS
preempts.** A thread runs until it blocks or yields `[HW wut/coreinit/thread.h]`:

> "Threads must voluntarily yield execution to other threads. […] The scheduler will only switch
> context when a thread blocks (e.g. on a mutex) or explicitly yields."

And `OSYieldThread` **only yields to threads of equal priority** — it never drops to a lower-priority
thread `[HW]`. A high-priority spinner that yields in a loop will starve everything below it, on
hardware and here.

This shapes real titles: they are written around explicit yield points, and their timing depends on
where those points fall. It is the reason the emulator's quantum jitter exists (below).

## Priorities and affinity

| | |
|---|---|
| Priority range | **0–31, 0 is highest** `[HW]` |
| Selection rule | lowest `effectivePriority` among READY threads on that core `[SRC coreinit_Thread.cpp:1190-1196]` |
| Base vs effective | `basePriority` and `effectivePriority` are separate fields `[SRC coreinit_Thread.h:430-431]` — effective is what the scheduler reads, allowing priority inheritance |

```cpp
// OSThread_t::ATTR_BIT   [SRC coreinit_Thread.h:392-399]
ATTR_AFFINITY_CORE0 = 0x1,
ATTR_AFFINITY_CORE1 = 0x2,
ATTR_AFFINITY_CORE2 = 0x4,
ATTR_DETACHED       = 0x8,
```

Affinity is a **mask**, so a thread may be eligible on several cores. `wut` also defines
`OS_THREAD_ATTRIB_AFFINITY_ANY` as the union of all three, and `OS_THREAD_ATTRIB_STACK_USAGE` for
stack-watermark tracking `[HW]`.

## Thread states

```cpp
// OSThread_t::THREAD_STATE   [SRC coreinit_Thread.h:378-385]
STATE_NONE = 0, STATE_READY = 1, STATE_RUNNING = 2, STATE_WAITING = 4, STATE_MORIBUND = 8
```

Matching `wut`'s documented set: READY, RUNNING, WAITING (blocked on a mutex/queue), MORIBUND (about
to terminate) `[HW]`.

`OSThread_t` is 0x6A0 bytes, validated by `MAGIC_THREAD = 0x74487244` — ASCII `"tHrD"`
`[SRC coreinit_Thread.h:369]`. `OSContext` carries its own magic pair `"OSCo"`/`"ntxt"`
(`0x4F53436F` / `0x6E747874`) `[SRC coreinit_Thread.h:16-17]`.

Fields worth knowing, because they are per-thread accounting the OS itself maintains:

| Field | Offset | Meaning |
|---|---|---|
| `suspendCounter` | `+0x328` | thread is suspended while > 0 |
| `effectivePriority` | `+0x32C` | what the scheduler compares |
| `basePriority` | `+0x330` | |
| `quantumTicks` | `+0x5F8` | run quantum |
| `wakeUpCount` | `+0x608` | times the thread entered RUNNING |
| `totalCycles` | `+0x610` | cycles this thread has been active since creation |

`[SRC coreinit_Thread.h:426-475]`. `wut` additionally documents `coreTimeConsumedNs` and
`coreRunQueue[3]`/`coreRunQueueLink[3]` `[HW]`.

## The API surface

| Call | Semantics |
|---|---|
| `OSCreateThread` / `OSCreateThreadType` | priority, attr mask, stack |
| `OSYieldThread` | yield to **equal** priority only |
| `OSSleepThread` / `OSWakeupThread` | block on a queue; wakeup releases *all* waiters |
| `OSSuspendThread` / `OSResumeThread` | counted suspend |
| `OSSetThreadPriority` / `OSSetThreadAffinity` | |
| `OSSetThreadRunQuantum` | max time before a forced yield `[HW]` |
| `OSTestThreadCancel` | implicitly called by mutex, spinlock and cancel operations |
| `OSSleepTicks` | timed sleep, backed by an alarm |

Plus the synchronisation set in `coreinit_Synchronization.cpp` (31 exports): mutexes, fast mutexes,
condition variables, semaphores, events, and `coreinit_MessageQueue.cpp` / `coreinit_MPQueue.cpp`
for message and multi-producer queues.

`OSThread` tracks the mutex it awaits and the mutexes it owns (`mutex`, `mutexQueue`), plus
`fastMutex`, `contendedFastMutexes`, `fastMutexQueue` `[HW]` — the data needed for priority
inheritance.

## How this emulator models it

### Guest threads are fibers, not host threads

```cpp
struct OSHostThread {            // [SRC coreinit_Thread.cpp:62-76]
    OSThread_t* m_thread;
    Fiber       m_fiber;
    uint8       padding[1024 * 128];   // 128 KB — used as the recompiler stack
    PPCInterpreter_t ppcInstance;      // per-guest-thread CPU state, by value
    uint32      selectedCore;
};
```

`Fiber` is `ucontext`-based with 2 MB stacks `[SRC FiberUnix.cpp]`. Because `makecontext` passes
`int` arguments, the 64-bit fiber parameter is split across two — hence
`__OSFiberThreadEntry(uint32 _high, uint32 _low)`.

`Fiber::Switch` `[SRC FiberUnix.cpp:46-53]` is the single narrowest choke point for *every* guest
context switch. On Darwin/arm64 `swapcontext` calls `sigprocmask`, costing ~200–500 ns on each of
save and restore `[SRC ../porting/02-cpu-jit-memory.md §4.2]` — a known, unaddressed overhead.

**Note for anyone adding fields to `PPCInterpreter_t`:** it is embedded by value here, so growing it
grows every live guest thread object.

### Two scheduling modes

`OSSchedulerBegin(numCPUEmulationThreads)` takes 1 or 3 `[SRC coreinit_Thread.cpp:1447-1467]`:

- **Multicore** (`g_isMulticoreMode == true`) — three host `std::thread`s, one per guest core, each
  pinned via `t_assignedCoreIndex`. Cores 0 and 2 block on a run-queue semaphore; **core 1 is the
  "main core"** and additionally runs `__OSCheckSystemEvents()`.
- **Singlecore** — one host thread rotating `coreIndex = (coreIndex + 1) % 3`
  `[SRC coreinit_Thread.cpp:1259]`.

> **Core-index trap.** In singlecore mode `t_assignedCoreIndex` is always 0 while the *logical* core
> rotates. The guest-visible core index is `hCPU->spr.UPIR`. **Use `UPIR` for guest-core attribution
> and `t_assignedCoreIndex` for host-thread attribution — never conflate them.**

### The timeslice

```cpp
uint32 ppcThreadQuantum = 45000;   // guest instructions   [SRC PPCScheduler.cpp:10]
```

Overridable per title via the game profile. `remainingCycles` (a `sint32` at offset 688 in
`PPCInterpreter_t`) is decremented in bulk by the JIT — one `ldr/sub/str` per basic block, emitted
by `PPCREC_IML_MACRO_COUNT_CYCLES` — and one at a time by the interpreter loop. **The sign bit is
the scheduling trigger.** An HLE call charges a flat 500 cycles `[SRC BackendAArch64.cpp:872]`.

The execution loop is short enough to quote in full `[SRC coreinit_Thread.cpp:1360-1391]`:

```cpp
while (true) {
    if (hCPU->remainingCycles > 0) {
        PPCRecompiler_attemptEnterWithoutRecompile(hCPU, hCPU->instructionPointer);
        while ((--hCPU->remainingCycles) >= 0)
            PPCInterpreterSlim_executeInstruction(hCPU);
    }
    hCPU->reservedMemAddr = 0; hCPU->reservedMemValue = 0;   // drop lwarx reservation
    __OSLockScheduler(); __OSThreadSwitchToNext(); __OSUnlockScheduler();
}
```

### The quantum jitter — do not remove it

```cpp
// __OSThreadStartTimeslice   [SRC coreinit_Thread.cpp:1165-1178]
hCPU->remainingCycles = ppcThreadQuantum;
// we add a slight randomized variance to the thread quantum to avoid getting stuck in repeated
// code sequences where one or multiple threads always unload inside a lock
// this was seen in Mario Party 10 during early boot where several OSLockMutex operations would
// align in such a way that one thread would never successfully acquire the lock
if (s_lehmer_lcg[coreIndex] == 0)
    s_lehmer_lcg[coreIndex] = 12345;
hCPU->remainingCycles += (s_lehmer_lcg[coreIndex] & 0x7F);
s_lehmer_lcg[coreIndex] = (uint32)((uint64)s_lehmer_lcg[coreIndex] * 279470273ull % 0xfffffffbull);
```

This is a **liveness fix, not noise**. Because Cafe OS scheduling is cooperative, a fixed quantum can
put a title into a stable pattern where one thread's yield always lands inside another's critical
section. Mario Party 10 livelocks on exactly that during boot.

Two things follow, and both matter for benchmarking:

1. **The jitter is already deterministic.** It is seeded to 12345 and advanced by a fixed
   Lehmer LCG, so for a given sequence of timeslices it produces the same sequence of values. It is
   not a randomness source.
2. **Pinning the quantum to remove it can hang titles.** Any "determinism mode" must keep the jitter
   and instead address the real non-determinism, which is host thread interleaving in multicore mode.

### The idle path

`__OSThreadCoreIdle` `[SRC coreinit_Thread.cpp:1237-1300]` is the per-core idle fiber. Its history is
instructive: it used to spin without yielding, and profiling attributed **67.6% of all emulator CPU
cycles** to `mach_continuous_time` called from it. It now parks on the run-queue semaphore with a
250 µs bound (`kIdleCorePollInterval` `[SRC :1229]`).

The main core additionally calls `__OSCheckSystemEvents()` `[SRC :1218-1226]` each pass, which drives
`AXOut_update()` (audio), `alarm_update()`, and `nnNfp_update()`. **Audio and alarms are therefore
paced by the idle loop**, which is worth remembering when reasoning about audio glitching under load.

### Context switch accounting

`__OSStoreThread` `[SRC coreinit_Thread.cpp:1113-1143]` already computes retired cycles:

```
executedCycles = quantumTicks - remainingCycles;   // clamped, minus skippedCycles
thread->totalCycles += executedCycles;
```

so **guest instructions retired is available at zero additional cost** — useful to know before
adding any JIT-side counter.

`__OSLoadThread` sets `spr.UPIR = coreIndex`, refreshes `quantumTicks`, sets
`wakeUpTime = PPCInterpreter_getMainCoreCycleCounter()` and increments `wakeUpCount`
`[SRC :1145-1163]`.

## What is not deterministic here

For anyone building replay or A/B tooling:

| Source | Deterministic? |
|---|---|
| Quantum jitter LCG | **Yes** — fixed seed, fixed recurrence |
| Thread selection given a run queue | **Yes** — lowest effective priority, first found |
| **Host thread interleaving (multicore)** | **No** — three real host threads, OS-scheduled |
| Singlecore rotation | **Yes** — fixed round-robin |
| Guest clock reads (`mftb`) | **No** — derived from the host counter |
| Vsync timing | **No** — software timer against wall clock |
| Alarm firing | **No** — paced by the idle loop |

Singlecore mode is therefore the only genuinely reproducible configuration — but it changes the
performance profile, so it is a **repro tool, not a benchmark configuration**.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| Cooperative scheduling semantics | **Modelled** | Yield/block-driven switching |
| Priorities 0–31, lowest-wins selection | **Modelled** | Linear scan of the per-core run queue |
| Per-core affinity mask | **Modelled** | |
| `effectivePriority` vs `basePriority` | **Modelled** | Priority inheritance representable |
| Suspend counting, cancel, join | **Modelled** | |
| Mutexes, fast mutexes, condvars, semaphores, events, message queues | **Modelled** | 31 exports in `coreinit_Synchronization.cpp` |
| Per-thread cycle accounting (`totalCycles`, `wakeUpCount`) | **Modelled** | |
| Timeslice length | **Approximated** | 45000 guest *instructions*, not cycles; no cache or stall modelling |
| Quantum jitter | **Emulator-specific** | Not hardware. A liveness workaround — see above |
| `OSSetThreadRunQuantum` | **Approximated** | Quantum is global, not per-thread |
| Context switch cost | **Approximated** | `swapcontext` + `sigprocmask`; unrelated to hardware cost |
| Thread execution on real cores | **Approximated** | Fibers on host threads; no true parallel core timing |
| Alarm/audio pacing | **Approximated** | Driven by the idle loop at ≥250 µs granularity |
| Decrementer-driven preemption | **Absent** | DEC is inert (ch. 01) |
| Priority-inheritance *timing* effects | **Absent** | Representable but not timing-accurate |
