# 04 — GX2 and the command model

How a guest `GX2SetDepthStencilControl()` becomes a Metal draw. This is the most
implementation-relevant chapter in the set: the telemetry and FIFO-replay work in
[`../../docs/porting/00-master-plan.md`](../porting/00-master-plan.md) both hook into this path.

```
guest GX2* call
   │  writes big-endian dwords through the per-core write-gather pointer
   ▼
command buffer  (suballocated from a 4 MB pool in guest memory)
   │  GX2Command_SubmitCommandBuffer builds a 4-9 word PM4 packet
   ▼
TCL ring buffer  (4096 dwords, lock-free, atomic read/write indices)
   │  LatteThread pulls words
   ▼
LatteCP_ProcessRingbuffer  →  LatteCP_processCommandBuffer  →  g_renderer->draw_execute
```

The crucial structural fact, and the one that shapes everything downstream: **the ring carries only
a pointer, not the commands.** A submission is 4–9 dwords containing a *physical address* and a
size. The actual PM4 stream lives in guest memory and can be rewritten by the CPU at any time.

## Write-gather

Each of the three guest cores has its own command-buffer write cursor:

```cpp
struct GX2PerCoreCBState {
    uint32be* bufferPtr;
    uint32    bufferSizeInU32s;
    uint32be* currentWritePtr;
    bool      isDisplayList;
};
extern GX2PerCoreCBState s_perCoreCBState[Espresso::CORE_COUNT];   // [SRC GX2_Command.h:7-15]
```

The variadic `gx2WriteGather_submit(...)` resolves `PPCInterpreter_getCurrentCoreIndex()` and writes
each argument as one big-endian dword, with overloads for `betype<T>`, `float`, and
`Latte::LATTEREG` (which writes `arg.getRawValue()`) `[SRC GX2_Command.h:24-84]`.

On hardware this path uses the CPU's **write-gather pipe** (chapter 01) — a store-combining channel
straight to the GPU FIFO. We do not model it; we just write to memory.

## The command pool

`GX2Init_commandBufferPool` allocates **4 MB by default** from the default heap at 0x100 alignment,
or takes a title-supplied buffer `[SRC GX2_Command.cpp:70-100]`:

```cpp
uint32 poolSize = bufferSize ? bufferSize : 0x400000;   // 4 MB
```

Individual command buffers are suballocated from it as a ring. `GX2Command_StartNewCommandBuffer`
rounds each request up to 8 dwords (**32-byte alignment**), with a floor of `0x100` dwords and a cap
of `0x20000`, and blocks in `GX2Command_WaitForNextBufferRetired()` when the pool is full
`[SRC GX2_Command.cpp:145-221]`.

The read cursor comes from `gpuCommandReadPtr` — **a value the GPU writes back into guest memory**
via the optional `IT_MEM_WRITE` in each submission. That is the flow-control loop.

> **Consequence for capture/replay:** because the pool is a 4 MB ring, **physical addresses are
> reused roughly every 4 MB of emitted PM4** — a fraction of a second in a real title. Any scheme
> that snapshots guest memory once and then replays ring words is not merely lossy, it is wrong.
> Command-buffer contents must be captured at submission time and versioned by content.

`GX2Command_PadCurrentBuffer` fills to the 8-dword boundary with `pm4HeaderType2Filler()` = `0x80000000`
`[SRC GX2_Command.cpp:243-262]`.

## The submission packet

```cpp
// GX2Command_SubmitCommandBuffer  [SRC GX2_Command.cpp:222-247]
cmd[0] = pm4HeaderType3(IT_INDIRECT_BUFFER_PRIV, 3);
cmd[1] = memory_virtualToPhysical(buffer);   // low 32 bits
cmd[2] = 0x00000000;                         // address high bits
cmd[3] = sizeInU32s;
// optional read-pointer writeback:
cmd[4] = pm4HeaderType3(IT_MEM_WRITE, 4);
cmd[5] = phys(completionGPUReadPointer) | 2;
cmd[6] = 0x40000;
cmd[7] = buffer + sizeInU32s;                // value the GPU writes back
cmd[8] = 0x00000000;
```

then `TCL::TCLSubmitToRing(cmd, cmdLen, &flags, &lastSubmissionTime)` with `USE_RETIRED_MARKER` set.

Header encoding `[SRC LattePM4.h:57-58]`:
```c
#define pm4HeaderType3(it, dwords)  (0xC0000000 | ((it)<<8) | (((dwords)-1)<<16))
#define pm4HeaderType2Filler()      (0x80000000)
```

## The TCL ring

`src/Cafe/OS/libs/TCL/TCL.cpp` — 4096 dwords, single producer (guest core), single consumer
(LatteThread):

```cpp
static constexpr uint32 TCL_RING_BUFFER_SIZE = 4096;                 // in U32s
std::atomic<uint32> tclRingBufferA[TCL_RING_BUFFER_SIZE];
std::atomic<uint32> tclRingBufferA_readIndex{0};
std::atomic<uint32> tclRingBufferA_writeIndex{0};                    // [SRC TCL.cpp:65-69]
```

Producer waits for space with a `_mm_pause` spin (`TCLWaitForRBSpace`); consumer parks with ARM
`ldxr` + `wfe` (`TCLGPUWaitForRBData`) rather than spinning. That change is documented in-source
with its measurements: a cross-thread store wakes the core in 42–208 ns, a pass costs ~1269 ns
instead of ~42 ns, giving ~30× fewer idle iterations `[SRC TCL.cpp:82-116]`. It is safe against a
guest that never submits again because Apple Silicon implements a **WFE timeout of ~1.3 µs**, so
`wfe` always returns on its own.

Submission flags `[SRC TCL.h:10-15]`:

| Flag | Value | Meaning |
|---|---|---|
| `SURFACE_SYNC` | `0x400000` | emit a surface-sync packet before the command |
| `NO_MARKER_INTERRUPT` | `0x200000` | do not raise an interrupt on retire |
| `USE_RETIRED_MARKER` | `0x20000000` | timestamp *after* retirement rather than before; selects which counter is returned |

TCL exports exactly three functions to the guest: `TCLSubmitToRing`, `TCLTimestamp`,
`TCLWaitTimestamp` `[SRC TCL.cpp:204-206]`.

### Retire markers and timestamps

`TCLSubmitToRing` appends a retire marker: `IT_EVENT_WRITE_EOP` with `EVENT_TYPE_TS (5)`, a 64-bit
write select (`0x40000000`), and an optional interrupt bit (`0x2000000`), targeting
`TCLStatePPC::gpuRetireMarker` — a `uint64be` **mapped into guest address space** so the CPU can poll
it `[SRC TCL.cpp:152-192]`.

GX2's timestamp API sits directly on top `[SRC GX2_Command.cpp:304-322]`:

| GX2 call | Implementation |
|---|---|
| `GX2GetLastSubmittedTimeStamp()` | atomic read of `s_commandState->lastSubmissionTime` |
| `GX2GetRetiredTimeStamp()` | `TCLTimestamp(TIMESTAMP_LAST_BUFFER_RETIRED, …)` |
| `GX2WaitTimeStamp(ts)` | `TCLWaitTimestamp(…, Espresso::TIMER_CLOCK * 60)` — **a 60-second GPU timeout expressed in guest timer ticks** |

## The PM4 packet set

`[SRC LattePM4.h]`, complete:

| Opcode | Value | Purpose |
|---|---|---|
| `IT_SET_PREDICATION` | `0x20` | conditional rendering |
| `IT_DRAW_INDEX_2` | `0x27` | indexed draw, indices in memory |
| `IT_CONTEXT_CONTROL` | `0x28` | selects register shadowing behaviour |
| `IT_INDEX_TYPE` | `0x2A` | 16- vs 32-bit indices |
| `IT_DRAW_INDEX_AUTO` | `0x2D` | non-indexed draw |
| `IT_DRAW_INDEX_IMMD` | `0x2E` | indexed draw, **indices inline in the packet** |
| `IT_NUM_INSTANCES` | `0x2F` | instance count |
| `IT_INDIRECT_BUFFER_PRIV` | `0x32` | execute a command buffer at a physical address |
| `IT_STRMOUT_BUFFER_UPDATE` | `0x34` | streamout cursor |
| `IT_MEM_SEMAPHORE` | `0x39` | GPU-side semaphore signal/wait |
| `IT_WAIT_REG_MEM` | `0x3C` | **GPU stalls until a memory location matches** |
| `IT_MEM_WRITE` | `0x3D` | GPU writes a value to memory |
| `IT_SURFACE_SYNC` | `0x43` | cache invalidation over an address range |
| `IT_EVENT_WRITE` / `_EOP` | `0x46` / `0x47` | pipeline events; EOP = end of pipe |
| `IT_LOAD_*_REG` | `0x60`–`0x66` | load register block from memory |
| `IT_SET_*_REG` | `0x68`–`0x6F` | write register block inline |
| `IT_STRMOUT_BASE_UPDATE` | `0x72` | |
| `IT_SET_ALL_CONTEXTS` | `0x74` | |

### Emulator-private opcodes

Cemu injects its own opcodes in the `0xEE`–`0xFB` range — these do **not** exist on hardware. They
are how HLE'd GX2 functions signal the command processor:

| Opcode | Value | Emitted by |
|---|---|---|
| `IT_HLE_COPY_SURFACE_NEW` | `0xEE` | `GX2CopySurface` |
| `IT_HLE_SYNC_ASYNC_OPERATIONS` | `0xEF` | |
| `IT_HLE_REQUEST_SWAP_BUFFERS` | `0xF0` | `GX2SwapScanBuffers` |
| `IT_HLE_WAIT_FOR_FLIP` | `0xF1` | `GX2WaitForFlip` |
| `IT_HLE_BOTTOM_OF_PIPE_CB` | `0xF2` | GX2 event callbacks |
| `IT_HLE_COPY_COLORBUFFER_TO_SCANBUFFER` | `0xF3` | |
| `IT_HLE_CLEAR_COLOR_DEPTH_STENCIL` | `0xF5` | `GX2ClearColor` etc. |
| `IT_HLE_SAMPLE_TIMER` | `0xF7` | |
| `IT_HLE_TRIGGER_SCANBUFFER_SWAP` | `0xF8` | `GX2SwapScanBuffers` — **the frame boundary** |
| `IT_HLE_SPECIAL_STATE` | `0xF9` | |
| `IT_HLE_BEGIN/END_OCCLUSION_QUERY` | `0xFA`/`0xFB` | |

Anything replaying a captured command stream must neutralise the blocking ones — `IT_WAIT_REG_MEM`,
`IT_MEM_SEMAPHORE`, `IT_HLE_WAIT_FOR_FLIP` all spin on state a replay will never produce.

## Display lists

A display list is just the write-gather pointer retargeted at title-supplied memory:
`GX2BeginDisplayList(Ex)`, `GX2EndDisplayList`, `GX2CallDisplayList`, `GX2DirectCallDisplayList`,
`GX2GetDisplayListWriteStatus` `[SRC GX2_Command.h:95-102]`. Buffers with `isDisplayList` set are
never submitted — `GX2Flush` on one logs and returns `[SRC GX2_Command.cpp:276]`.

The command processor supports **4 levels of nesting**, via a `static_vector<CmdQueuePos, 4>` in
`DrawPassContext` `[SRC LatteCommandProcessor.cpp:151]`.

`[HW]`: the SDK forbids non-display-list GX2 calls inside a display list and asserts on it. Also,
"wait" semaphores could bypass preceding "signal" semaphores and hang — fixed in SDK 2.07.02.

## Consumption: the command processor

`LatteCP_ProcessRingbuffer()` `[SRC LatteCommandProcessor.cpp:1450-1757]` is a never-returning loop
on the dedicated `LatteThread`. It reads a PM4 header, copies `nWords` into a 128-dword stack
buffer, and dispatches on `itCode`.

`IT_INDIRECT_BUFFER_PRIV` routes to `LatteCP_itIndirectBufferDepr` `[SRC :228-250]`, which is worth
quoting because it is the hinge of the whole design:

```cpp
uint32 physicalAddress = LatteReadCMD();
uint32 physicalAddressHigh = LatteReadCMD();   // unused
uint32 sizeInU32s = LatteReadCMD();
if (sizeInU32s > 0) {
    DrawPassContext drawPassCtx;
    uint32be* buf = MEMPTR<uint32be>(physicalAddress).GetPtr();
    drawPassCtx.PushCurrentCommandQueuePos(buf, buf, buf + sizeInU32s);
    LatteCP_processCommandBuffer(drawPassCtx);
    ...
}
```

`LatteCP_processCommandBuffer` walks a **plain host pointer range**, and
`PushCurrentCommandQueuePos` is public. That means a replay driver can feed it a captured *copy* of
a command buffer and reuse the real interpreter, rather than writing a second PM4 decoder that could
drift.

There are two parsers:

- **`LatteCP_processCommandBuffer`** `[SRC :1172-1448]` — the generic PM4 loop.
- **`LatteCP_processCommandBuffer_continuousDrawPass`** `[SRC :1007-1170]` — a fast path entered
  immediately after a draw, implementing only the commands that do not disturb pipeline state, and
  bailing back to the generic parser on anything else. This is a performance-critical structure:
  it decides when a render pass breaks, which is exactly the metric the graphics workstream is
  trying to reduce.

All three draw packet types funnel through one function:

```cpp
// DrawPassContext::executeDraw   [SRC LatteCommandProcessor.cpp:65-93]
g_renderer->draw_execute(...);
performanceMonitor.cycle[...].drawCallCounter++;
if (!m_drawcallContext.isFirst)
    performanceMonitor.cycle[...].fastDrawCallCounter++;
```

## Cache coherency contract

The GPU is not in the CPU's coherency domain (chapter 01). The contract is:

| Direction | Guest must call | We do |
|---|---|---|
| CPU wrote data the GPU will read | `DCFlushRange` + `GX2Invalidate` | `LatteBufferCache_notifyDCFlush` marks pages dirty |
| GPU wrote data the CPU will read | `GX2Invalidate` / wait on a timestamp | readback path |

`[HW]` notes on `GX2Invalidate`: it is required for coherence with cached memory, particularly for
`GX2TempAlloc*` allocations; non-aligned data needs an explicit `GX2Invalidate(address, size)`; and
changing to `GX2_SHADER_MODE_UNIFORM_BLOCK` or `_GEOMETRY_SHADER` required
`GX2Invalidate(GX2_INVALIDATE_SHADER, 0, 0xffffffff)` or draws would misbehave (SDK 2.07.03, later
automated).

Alignment requirements `[HW]`: `GX2_VERTEX_BUFFER_ALIGNMENT`, `GX2_INDEX_BUFFER_ALIGNMENT`,
`GX2_DEFAULT_BUFFER_ALIGNMENT`. DMAE copy/fill alignment was reduced from 8 to 4 bytes in SDK
2.09.07 "to match hardware requirements".

MEM1 has an extra rule: `GX2RDestroyBuffer` must not be called on MEM1 surfaces during shutdown;
use `GX2RDestroyBufferEx` with `GX2R_OPTION_NO_TOUCH_DESTROY`, and route all MEM1 allocation through
`GX2NotifyMemAlloc`/`GX2NotifyMemFree` `[HW]`.

## GPU hang detection and performance counters

Hardware and SDK provide both, and we implement neither.

**Hang detection** `[HW]`: `GX2SetGPUTimeout`, `GX2GetGPUTimeout`, `GX2PrintGPUStatus`, and from SDK
2.09.00 automatic detection and reset via `GX2SetMiscParam` / `GX2GetMiscParam` / `GX2ResetGPU`.

**Performance counters** `[HW]`: the SDK exposes a low-level perf API that separates counter
configuration from results buffers, decodes counter and stat names, combines multi-unit counters,
and gives access to all pipeline-stage statistics. Plus `GX2SampleTopGPUCycle` and
`GX2SampleBottomGPUCycle` using `GX2_TOP_BOTTOM_CLOCK_CYCLES`, and an optional frame-coherence check.

This matters for two reasons. First, a title that calls the perf API gets nothing useful from us.
Second — and more usefully — it tells us what the hardware itself considered worth measuring, which
is a reasonable prior for our own telemetry.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| Write-gather command emission | **Modelled** | Per-core cursors, big-endian dwords |
| Command pool ring + flow control | **Modelled** | 4 MB default, GPU writes back the read pointer |
| PM4 packet decode | **Modelled** | Full opcode set + emulator-private range |
| Display lists incl. 4-deep nesting | **Modelled** | |
| TCL ring | **Modelled** | 4096 dwords, `ldxr`/`wfe` consumer |
| Retire markers / timestamps | **Approximated** | EOP marker modelled; `GX2WaitTimeStamp` timeout hardcoded to 60 s |
| `IT_SURFACE_SYNC` | **Approximated** | Drives buffer-cache invalidation |
| Write-gather pipe as hardware | **Absent** | HLE'd; SDK-era hardware bug and workaround not reproduced |
| CPU/GPU cache coherency | **Approximated** | Shared memory is trivially coherent; we consume flush notifications only |
| `DCStoreRange` → cache notify | **Absent** | Commented out → ch. 09 |
| GPU hang detection / reset | **Absent** | `GX2SetGPUTimeout` etc. not implemented |
| GX2 performance counters | **Absent** | Including `GX2Sample{Top,Bottom}GPUCycle` |
| Tiling aperture ops | **Approximated** | 6 unimplemented sites in `GX2_TilingAperture.cpp` |
| `TCLSubmitToRing` without `USE_RETIRED_MARKER` | **Absent** | `cemu_assert_unimplemented()` at `TCL.cpp:189` |
| Guard-band / vertex-reuse behaviour | **Absent** | Host GPU characteristics differ entirely |
