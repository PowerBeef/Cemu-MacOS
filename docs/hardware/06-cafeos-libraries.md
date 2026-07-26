# 06 — Cafe OS libraries and the HLE mechanism

Cafe OS ships its system libraries as **RPL** modules (a modified ELF with compressed sections and
Windows-style dynamic linking); a title is an **RPX**, the same format. `coreinit.rpl` loads first,
before the main executable, because everything else depends on it for memory and thread primitives
`[RE]`.

This emulator implements those libraries at **high level (HLE)**: the guest never executes Nintendo's
library code. A guest call to `OSGetTime` lands in C++.

## How a guest call reaches C++

Four steps, and the trick in the middle is a good one.

### 1. Registration

```cpp
#define cafeExportRegister(__libname, __func, __logtype) { ... \
    cafeExportMakeWrapper<__func, StringWrapper, __logtype>(__libname, #__func); }
```
`[SRC OSUtil.h:220-236]`

`cafeExportMakeWrapper` deduces the C++ function signature and instantiates
`cafeExportCallWrapper<fn, …>` `[SRC OSUtil.h:168-210]`, which builds a `std::tuple<Args...>` from
the guest register file, `std::apply`s the real function, writes the result back, and finally sets
`hCPU->instructionPointer = hCPU->spr.LR` — i.e. returns to the guest.

The ABI it encodes is PPC EABI `[SRC OSUtil.h:13-104]`:

- integer/pointer arguments in `r3`–`r10`; beyond that, read from `[r1 + 8 + (n-8)*4]`
- 64-bit integers align the GPR index to even and occupy two registers, high then low
- floats/doubles in `f1`–`f8`
- returns: 32-bit in `r3`; 64-bit as `r3` = high, `r4` = low
- variadics via `ppc_va_list{gprIndex, fprIndex, overflow_arg_area, reg_save_area}`, with
  8 GPR saves (32 B) + 8 FPR saves (64 B), `static_assert(sizeof == 0x70)` `[SRC OSCommon.h:36-115]`

A legacy path also exists: `osLib_addFunction` with a raw `void(*)(PPCInterpreter_t*)` reading
arguments through `ppcDefineParamU32`/`MEMPTR`/`Str`/… macros `[SRC PPCState.h:130-144]` and
returning via `osLib_returnFromFunction`.

### 2. Name hashing

Each registration hashes the library and function names with a custom two-part rolling hash
(seeds `0x688BA2BA`, `0xF64A71D5`) into `s_osFunctionTable` `[SRC OSCommon.cpp:60-99]`. Data exports
use a parallel table via `osLib_addVirtualPointer` / `osLib_getPointer`.

### 3. Index assignment

Every wrapper gets a slot in a flat table, deduplicating identical function pointers:

```cpp
static constexpr size_t HLE_TABLE_CAPACITY = 0x4000;
HLECALL s_ppcHleTable[HLE_TABLE_CAPACITY]{};
HLEIDX PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName);
```
`[SRC PPCInterpreterHLE.cpp:20-45]`

> **`hleName` is accepted and then discarded** — the parameter is unused in the function body. The
> names are therefore available at registration time but thrown away, which is why nothing in the
> emulator can currently report *which* HLE functions a title called. Storing them costs nothing at
> runtime. → chapter 09.

### 4. The trap: primary opcode 1

The RPL loader synthesises a **one-instruction trampoline** per import:

```cpp
MPTR codeAddr = ...RPLLoader_AllocateTrampolineCodeSpace(4);
uint32 opcode = (1 << 26) | functionIndex;
memory_write<uint32>(codeAddr, opcode);            // [SRC rpl.cpp:771-776]
```

**PowerPC primary opcode 1 is unallocated on real hardware**, so Cemu repurposes it as the HLE trap
(`Espresso::PrimaryOpcode::VIRTUAL_HLE = 1` `[SRC EspressoISA.h:29]`). The low 16 bits carry the
table index. A guest `bl` into an imported function therefore executes exactly one instruction that
does not exist on PowerPC, and both execution engines intercept it:

- **Interpreter** — `case 1:` in the opcode-major switch → `PPCInterpreter_virtualHLE`
  `[SRC PPCInterpreterImpl.cpp:463-465, PPCInterpreterHLE.cpp:56-72]`
- **Recompiler** — `PPCRecompilerImlGen_HLE` → `PPCREC_IML_MACRO_HLE`, lowered to a call into
  `PPCRecompiler_virtualHLE` `[SRC BackendAArch64.cpp:866-884]`

Each HLE call is charged a flat **500 guest cycles** `[SRC BackendAArch64.cpp:872]`.

### Unresolved imports

Index `0xFFD0` is reserved. For an import that resolves to nothing, the loader writes the trap, a
`blr` (`0x4E800020`), and then the ASCII `"lib.func"` string inline `[SRC rpl.cpp:792-800]`:

```
+0:  (1 << 26) | 0xFFD0      // HLE trap, unsupported-import sentinel
+4:  0x4E800020              // blr
+8:  "coreinit.SomeFunction\0"
```

At runtime the interpreter reads that string back from `instructionPointer + 8`, logs it **once** to
`LogType::UnsupportedAPI`, sets `gpr[3] = 0` and continues `[SRC PPCInterpreterHLE.cpp:7-18]`.

> **Accuracy blind spot.** The *recompiler* path does not do this. `PPCRecompiler_virtualHLE` handles
> `hleFuncId == 0xFFD0` by charging 500 cycles, setting `gpr[3] = 0`, and returning — **with no
> logging at all** `[SRC BackendAArch64.cpp:870-876]`. Since the recompiler is the default CPU mode,
> unresolved imports are silently swallowed in normal operation, and the `UnsupportedAPI` log
> under-reports. → chapter 09.

### Host → guest calls

`makeCallableExport<fn>()` `[SRC OSUtil.h:238-242]` → `RPLLoader_MakePPCCallable`, used for callbacks
the guest registers with us and we must invoke.

`osLib_registerHLEFunction` is exported `extern "C" DLLEXPORT` `[SRC OSCommon.cpp:100-104]` — the
Cemuhook plugin ABI.

## The libraries

47 directories under `src/Cafe/OS/libs/`, **1178 `cafeExportRegister` call sites** plus ~431 legacy
`osLib_addFunction` registrations. The module registry `GetCOSModules()` returns 46 `COSModule*`
entries `[SRC COSModule.cpp:52-104]`.

Counts below are registration call sites; some libraries register the same function under two RPL
names (`snd_core`/`sndcore2`, `snd_user`/`snduser2`), so the distinct-export count is lower.

### The two that matter most

| Library | Exports | Role |
|---|---|---|
| **`coreinit`** | **452** | The kernel-facing library — threads, scheduler, MEM heaps, alarms, time, filesystem, IPC/IOS, MCP, DynLoad, atomics, spinlocks, locked cache, OSScreen, MMIO |
| **`gx2`** | **214** | The graphics API (chapters 03–04) |

`coreinit` by file, largest first `[SRC]`:

```
coreinit_FS.cpp             92      coreinit_Spinlock.cpp        9
coreinit_Thread.cpp         33      coreinit_IM.cpp              9
coreinit_Synchronization.cpp 31     coreinit_OSScreen.cpp        8
coreinit_MCP.cpp            22      coreinit_MEM_BlockHeap.cpp   8
coreinit_Misc.cpp           21      coreinit_HWInterface.cpp     7
coreinit_GHS.cpp            20      coreinit_Alarm.cpp           7
coreinit_MEM_ExpHeap.cpp    19      coreinit_Time.cpp            6
coreinit_MPQueue.cpp        18      coreinit_MessageQueue.cpp    6
coreinit_MEM.cpp            17      coreinit_MEM_UnitHeap.cpp    6
coreinit.cpp                15      coreinit_IPC.cpp             6
coreinit_Memory.cpp         15      coreinit_Scheduler.cpp       5
coreinit_DynLoad.cpp        13      coreinit_MemoryMapping.cpp   5
coreinit_LockedCache.cpp    12      coreinit_FG.cpp              4
coreinit_Atomic.cpp         12      coreinit_CodeGen.cpp         4
coreinit_MEM_FrmHeap.cpp    10      …
```

`coreinit_FS.cpp` is 2800+ lines and by far the largest single library file — the full `FS*` sync and
async API plus a complete `FSA*` layer.

**MEM heaps** `[SRC coreinit_MEM.h:52-100]` — four implementations, identified by magic:

| Heap | Magic | File |
|---|---|---|
| Expanded | `'EXPH'` | `coreinit_MEM_ExpHeap.cpp` |
| Frame | `'FRMH'` | `coreinit_MEM_FrmHeap.cpp` |
| Unit | `'UNTH'` | `coreinit_MEM_UnitHeap.cpp` |
| Block | `'BLKH'` | `coreinit_MEM_BlockHeap.cpp` |
| User | `'USRH'` | — |

Default alignment 4; options `CLEAR(1)`, `FILL(2)`, `THREADSAFE(4)`.

**Time** `[SRC coreinit_Time.cpp]` — `OSGetSystemTime` (timebase), `OSGetTime` (timebase + ticks
since 2000), `OSGetSystemTick`, `OSGetTick`, and calendar conversion with leap-year tables.
`coreinit_GetMFTB()` is `PPCInterpreter_getMainCoreCycleCounter() / 20` — the core/20 ratio from
chapter 01.

### The rest

| Library | Exports | Emulates |
|---|---|---|
| `snd_core` (+`sndcore2`) | 139 sites | AX audio mixer / DSP |
| `nn_olv` | 126 | Miiverse |
| `nn_boss` | 80 | Background download scheduler |
| `snd_user` (+`snduser2`) | 61 sites | MIDI/sequencer over AX |
| `nn_act` | 44 | NNID accounts |
| `nn_fp` | 44 | Friends / presence |
| `nsysnet` | 42 | BSD sockets + NSSL (TLS) |
| `nn_save` | 40 | Save-data filesystem |
| `vpad` | 36 | GamePad input |
| `padscore` | 30 | KPAD/WPAD — Wiimote, Classic, Pro |
| `nlibcurl` | 25 | libcurl shim |
| `nn_nfp` | 25 | amiibo / NFC |
| `nn_ac` | 20 | Network auto-connect |
| `proc_ui` | 19 | Foreground/background lifecycle |
| `nn_acp` | 18 | Application control / title metadata |
| `h264_avc` | 17 | H.264 decoder |
| `swkbd` | 17 | Software keyboard applet |
| `sysapp` | 16 | System applet launching |
| `erreula` | 15 | Error viewer applet |
| `zlib125` | 15 | zlib 1.2.5 |
| `nfc`, `ntag` | 13, 11 | NFC |
| `nn_nim` | 13 | Install manager |
| `nsyshid` | 9 | USB HID (portals, etc.) |
| `mic`, `camera` | 7, 7 | |
| `nn_spm`, `nn_ndm` | 6, 6 | Storage / network daemon |
| `dmae`, `avm`, `nn_aoc` | 5, 5, 5 | DMA engine, A/V manager, DLC |
| `drmapp` | 4 | DRM |
| **`TCL`** | **3** | GPU ring submission (chapter 04) |
| `nn_pdm` | 3 | Play diary |
| `nlibnss`, `nsyskbd`, `nn_cmpt`, `nn_idbe` | 2 each | |
| `nn_ec`, `nn_ccr`, `nn_sl`, `nn_temp`, `nn_uds` | 1 each | |

## Where we are incomplete

There is no `logStub`/`UNIMPLEMENTED` macro in this codebase. The markers are
`cemu_assert_unimplemented()`, `cemu_assert_debug(false)` / `assert_dbg()` /
`cemu_assert_suspicious()`, and `cemuLog_logDebug(..., "… stub")`.

**65 `cemu_assert_unimplemented()` sites under `src/Cafe/OS/`:**

| Library | Count | Concentrations |
|---|---|---|
| coreinit | 21 | **`coreinit_FS.cpp` 9**; `coreinit_MEM.cpp` 3 (two are "foreground required"); `coreinit_IPC.cpp` 2 (one reads *"we should wait for an event instead of busylooping"*) |
| gx2 | 11 | **`GX2_TilingAperture.cpp` 6**, `GX2_AddrTest.cpp` 2 |
| sysapp | 5 | |
| h264_avc | 4 | |
| nn_spm / nn_olv / nlibcurl | 3 each | |
| nn_nim / nn_boss | 2 each | |
| vpad / TCL / nn_save / nn_act / mic | 1 each | `TCL.cpp:189` — submission without `USE_RETIRED_MARKER` |

**260+ debug asserts**, dominated by `nsysnet` 74 (the sockets layer bails on most unhandled options),
`coreinit` 62, `h264_avc` 40 (the decoder is largely a shell), `snd_core` 33, `gx2` 29.

**Named stubs** worth knowing: `GX2CheckSurfaceUseVsFormat` `[SRC GX2_Surface.cpp:278]`,
`OSReleaseForeground` `[SRC coreinit_Misc.cpp:796]`, `OSDriver_Register`/`_Deregister` `[:850,:856]`,
`MCP_UpdateClearContextAsync` `[SRC coreinit_MCP.cpp:392]`, `GetBossState` `[SRC nn_boss.cpp:93]`,
`NSSExportDeviceCertChain` `[SRC nlibnss.cpp:8]`.

`todo` density per library — a rough proxy for known-incomplete: coreinit 58, gx2 46, nn_boss 40,
nsysnet 28, snd_core 23, h264_avc 17, snd_user 12, nn_nfp 10, nn_olv 9, vpad 7.

## Modelled / Approximated / Absent

| Behaviour | Status | Note |
|---|---|---|
| RPL/RPX loading, dynamic linking | **Modelled** | |
| PPC EABI argument marshalling | **Modelled** | Including variadics |
| HLE trap via opcode 1 | **Emulator-specific** | Not hardware; unallocated opcode repurposed |
| Unresolved-import reporting | **Approximated** | Interpreter logs once; **recompiler path silent** → ch. 09 |
| Named HLE call attribution | **Absent** | `hleName` accepted and discarded → ch. 09 |
| `coreinit` threads/sync/heaps/time | **Modelled** | See ch. 05 |
| `coreinit` filesystem | **Approximated** | 9 unimplemented sites, the most of any file |
| `gx2` | **Approximated** | 11 unimplemented sites, 6 in the tiling aperture |
| `nsysnet` | **Approximated** | 74 debug asserts on unhandled options |
| `h264_avc` | **Absent-ish** | 40 asserts; largely a shell |
| `snd_core` / `snd_user` | **Approximated** | See ch. 07 |
| HLE call cost | **Approximated** | Flat 500 guest cycles regardless of the work done |
| `nn_*` online services | **Approximated** | Varies widely by service |
