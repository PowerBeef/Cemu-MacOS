# 09 — Accuracy gap register

Every known divergence between this emulator and the hardware, in one place, prioritised.

This is the payoff chapter. It exists so that gaps are **enumerated rather than rediscovered**, and
it is the input to the telemetry harness's accuracy signals: a row marked *detectable* is one the
emulator should be able to count and name at runtime, turning "we probably get this wrong" into
"this title hit it 1,840 times last frame".

## Measured, BotW at the Shrine of Resurrection

The telemetry harness now counts these at runtime. First full run, 9,555 frames:

| Signal | Per frame | Run total | Reading |
|---|---|---|---|
| `acc.unsupported_hle_calls` | **557** | 3,696,250 | 31 distinct functions — see 4.2 |
| `acc.audio_frames_serviced` | 11.0 | 111,246 | **Exactly the expected rate** — see 2.5 |
| `acc.audio_update_polls` | 232 | 2,175,964 | 21× oversampled, harmless |
| `acc.d24_s8_use` | 0 | 0 | BotW is not affected by 1.1 |
| `acc.dcstorerange_no_notify` | 0 | 0 | BotW never takes the 1.3 path |
| `acc.geometry_draw_dropped` | 0 | 0 | Confirms the branch is dead on Apple Silicon |
| `acc.unsupported_tex_format` | 0 | 0 | |
| `acc.unsupported_primitive` | 0 | 0 | |
| `acc.draw_skipped_no_target` / `_shader_err` | 0 | 0 | |

**Only one gap in this register actually fires in BotW, and it fires 557 times a frame.**
Every other one is either absent from this title's workload or, in the geometry-shader case,
confirmed dead. That is the point of measuring rather than reasoning: a list of eleven
plausible problems turned out to be one real one, and the real one was previously invisible.

These figures are for one scene in one title. A different title will light up different rows.

---

Severity is about **observable consequence**, not effort:

| | Meaning |
|---|---|
| **S1** | Wrong rendering or wrong results, on hardware we ship to, today |
| **S2** | Architecturally wrong; no title known to notice, but the divergence is real |
| **S3** | Behaviour absent; nothing currently observes it |
| **S4** | Emulator-side defect (reporting, tooling) rather than a hardware divergence |

---

## S1 — wrong today

### 1.1 `D24_S8` depth decoder is disabled on every Apple GPU

```cpp
if (!support.m_supportsDepth24Unorm_Stencil8) {
    MTL_DEPTH_FORMAT_TABLE[D24_S8_UNORM].pixelFormat = MTL::PixelFormatDepth32Float_Stencil8;
    // TODO: implement the decoder
    //MTL_DEPTH_FORMAT_TABLE[D24_S8_UNORM].textureDecoder = TextureDecoder_D24_S8_To_D32_S8::getInstance();
}
```
`[SRC LatteToMtl.cpp:173-179]`

`depth24Stencil8PixelFormatSupported` is **false on all Apple Silicon**, so this branch is taken
unconditionally on the only hardware this fork supports. The format is remapped but the data is not
converted. Any title uploading or reading back `D24_S8` depth gets garbage.

**Detectable:** yes — count uses of `D24_S8_UNORM` reaching the depth format table.

### 1.2 Render-pass self-dependency handling is commented out

The entire accurate-barriers block in `MetalRenderer::draw_execute` is inside `/* … */`
`[SRC MetalRenderer.cpp:1133-1163]`. That includes `CheckIfRenderPassNeedsFlush` for all three
shader stages **and two title-specific workarounds that were written because they were needed**:

```cpp
if (pixelShader->baseHash == 0x6f6f6e7b9aae57af && ...) // BotW lava
    neverSkipAccurateBarrier = true;
if (pixelShader->baseHash == 0x4c0bd596e3aef4a6 && ...) // BotW foam layer for water at waterfall bottoms
    neverSkipAccurateBarrier = true;
```

A shader that samples the render target it is writing reads undefined data. Two BotW effects are
known to hit this. The Vulkan design that solved it is preserved in
[`../porting/ref-vulkan-self-dependency.md`](../porting/ref-vulkan-self-dependency.md) and has not
been ported.

**Detectable:** yes — the disabled check *is* the detector. Re-enable it in counting-only mode
(count, do not flush) to measure how often it fires before paying for the fix.

### 1.3 `DCStoreRange` does not invalidate the GPU buffer cache

```cpp
void DCStoreRange(MPTR addr, uint32 size) {
    MPTR addrEnd = (addr + size + 0x1F) & ~0x1F;
    addr &= ~0x1F;
    //LatteBufferCache_notifyDCFlush(addr, addrEnd - addr);   // <-- commented out
}
```
`[SRC coreinit_Memory.cpp:33-38]`

`DCStoreRange` writes dirty cache lines back to memory — precisely the case the GPU-side cache needs
to hear about. Its own `NoSync` variant *does* notify `[SRC :40-45]`. The asymmetry has no defensible
reading. A title that uses `DCStoreRange` before a draw can have the GPU read stale vertex or uniform
data.

(`DCInvalidateRange` also has the call commented out, and that one is *correct* — invalidate discards
the CPU's copy without writeback, so there is nothing new for the GPU to see.)

**Detectable:** yes — count `DCStoreRange` calls that overlap a live buffer-cache range.

---

## S2 — architecturally wrong

### 2.1 `fmadd` double-rounds

PPC `fmadd` is a fused multiply-add: one rounding, at the end. Our lowering computes
`round(round(a·c) + b)`. For `fmadds`, hardware performs the fused operation at double precision and
*then* rounds to single. Detail, and the `fmsub` ↔ `fnmsub` mnemonic crossover trap, in
`[SRC ../porting/02-cpu-jit-memory.md §3.2]`.

Consequence: last-ULP divergence in float results. Titles that hash floats or accumulate physics can
desync replays and ghosts.

**Detectable:** partially — a differential fuzzer against the interpreter catches it offline; runtime
detection is impractical.

### 2.2 `frC` is not rounded to 25 bits

Real Espresso rounds the `frC` operand of a paired-single multiply to 25 bits before multiplying — a
Gekko quirk. Three `todo` sites acknowledge it `[SRC PPCRecompilerImlGenFPU.cpp:1025, 1065, 1171]`.
Deliberately not implemented; the cost is high and no title is known to depend on it.

**Detectable:** no, not at acceptable cost.

### 2.3 Guest timebase resolution is capped by the host counter

Guest timebase is 62.15625 MHz (16.09 ns); host `cntvct_el0` is 24 MHz (41.67 ns). **The host clock
is 2.6× coarser than the clock we emulate**, and there is no finer clock on the platform
`[SRC ../porting/02-cpu-jit-memory.md §4.1]`. A title measuring a sub-42 ns interval cannot get a
correct answer.

**Detectable:** no — it is a resolution floor, not an event.

### 2.4 HLE calls cost a flat 500 guest cycles

`[SRC BackendAArch64.cpp:872]`, regardless of whether the call is `OSGetTime` or a filesystem read.
Titles that budget CPU time around library-call cost see a uniform, wrong figure.

**Detectable:** yes — the HLE histogram gives call counts per function; combined with host timing it
shows where the flat charge is most wrong.

### 2.5 Audio is paced by the idle loop, not a 3 ms clock — **measured, and not a problem here**

AX runs on a 3 ms frame (chapter 07), but `AXOut_update()` is driven from `__OSCheckSystemEvents()`
on the main core's **idle loop**, which parks with a 250 µs bound
`[SRC coreinit_Thread.cpp:1218-1226, :1229]`. The hypothesis was that under load the servicing rate
degrades, explaining audio glitching that correlates with frame drops.

**Measured on the BotW shrine: it does not.** `acc.audio_frames_serviced` is **11.0 per video
frame**, and 333 Hz ÷ 30 fps = 11.1 — audio is serviced at exactly the right rate.

What the measurement did show is that `AXOut_update` is *polled* 232 times per frame and does work
11 times, because it early-returns from two gates before touching anything. A 21× oversample of a
cheap function: not a correctness issue, and not obviously worth changing.

Downgraded from "plausible explanation" to "not supported by evidence" for this scene. It could
still bite a title that saturates the main core harder than BotW does; the counters are in place to
catch that if it happens.

**Note on how this was nearly measured wrong:** the counter was first placed at the top of
`AXOut_update`, which is *before* both early-return gates, and it read 232/frame — 20× the expected
rate. That looks exactly like the problem the hypothesis predicted, and it would have confirmed the
hypothesis while actually measuring the poll rate. The name promised "frames serviced"; the site
delivered "calls". Both counters exist now precisely so the distinction stays visible.

### 2.6 No IPC latency

Every filesystem, socket and crypto operation is an IPC round trip to IOSU on hardware (chapter 08);
here they are direct C++ calls. Titles that overlap I/O with computation see different timing.

**Detectable:** yes, cheaply — count IPC-surface calls; the *latency* is the absent part.

---

## S3 — absent, nothing observes it today

| Gap | Evidence |
|---|---|
| Decrementer inert — read path opens with `assert_dbg()`, nothing raises the exception | `[SRC PPCInterpreterSPR.hpp:813-822]` |
| `PVR` returns `0x70010101`, comment says **"guessed"** | `[SRC PPCInterpreterSPR.hpp:58-61]` |
| `HID0`,`HID1`,`HID2`,`HID4`,`HID5` all return 0 | `[SRC :99, :110, :116, :128, :140]` |
| No CPU cache model at all — no L1, L2, line fills, or coherency traffic | ch. 01 |
| MEM1 has no bandwidth or latency advantage over MEM2 | ch. 02 |
| `VI` register block is an empty stub; `AI.h` is an empty file | `src/Cafe/HW/VI/VI.cpp`, `AI/AI.h` |
| GX2 GPU hang detection (`GX2SetGPUTimeout`, `GX2ResetGPU`) | ch. 04 |
| GX2 performance counters (`GX2Sample{Top,Bottom}GPUCycle`, pipeline stats) | ch. 04 |
| `TCLSubmitToRing` without `USE_RETIRED_MARKER` | `[SRC TCL.cpp:189]` — `cemu_assert_unimplemented()` |
| Tiling aperture operations — 6 unimplemented sites | `GX2_TilingAperture.cpp` |
| Write-gather pipe as hardware, incl. its documented corruption bug | ch. 01, 04 |
| No compiled-shader disk cache — every shader recompiles per launch | ch. 03 |

---

## S4 — emulator-side defects

### 4.1 `l2cacheSize[1]` reports 3.76 MB instead of 2 MB

```cpp
g_system_info->l2cacheSize[1] = 2*1024*1924; // 2MB
```
`[SRC coreinit_SystemInfo.cpp:21]` — `1924` is a typo for `1024`. `OSGetSystemInfo()` returns
3,940,352 where hardware reports 2,097,152. A one-character fix.

### 4.2 Unresolved imports are silently swallowed under the recompiler

The interpreter's `PPCInterpreter_handleUnsupportedHLECall` reads the `"lib.func"` string the RPL
loader embedded after the trap and logs it once to `LogType::UnsupportedAPI`
`[SRC PPCInterpreterHLE.cpp:7-18]`.

The **recompiler does not**:

```cpp
if (hleFuncId == 0xFFD0) {
    ppcInterpreter->remainingCycles -= 500;
    ppcInterpreter->gpr[3] = 0;
    PPCInterpreter_nextInstruction(ppcInterpreter);
    return PPCInterpreter_getCurrentInstance();      // no logging at all
}
```
`[SRC BackendAArch64.cpp:870-876]`

The recompiler is the **default** CPU mode, so in normal operation unresolved imports produced no
diagnostic whatsoever, and the `UnsupportedAPI` log under-reported by an unknown margin.

**Fixed.** Both paths now route through `PPCInterpreter_handleUnsupportedHLECall`, and the margin
turned out to be the whole thing. BotW at the shrine calls unimplemented functions **557 times per
frame** — 3.7 million times in a 9,555-frame run — across **31 distinct imports**:

| Library | Distinct unresolved imports | Examples |
|---|---|---|
| `snd_core` | **13** | `AXSetVoicePriority`, `AXSetVoiceRmtOn`, `AXSetVoiceRmtIIR`, `AXSetDRCVSSurroundDepth`, `AXGetSwapProfile` |
| `coreinit` | 6 | **`OSCoherencyBarrier`**, `FSSetStateChangeNotification`, `OSGetShutdownReason`, `UCOpen`/`UCClose` |
| `gx2` | 6 | `GX2SetLineWidth`, `GX2ExpandDepthBuffer`, `GX2SetAlphaToMaskReg`, `GX2SetTVScale` |
| `padscore` | 2 | `WPADEnableURCC`, `KPADSetMplsWorkarea` |
| `vpad` | 2 | `VPADEnableGyroAccRevise`, `VPADSetGyroAccReviseParam` |
| `nn_aoc` | 2 | `AOC_Initialize` |

Two of these deserve follow-up on their own. **`coreinit.OSCoherencyBarrier` is a coherency
primitive** and a title calling it is telling us exactly where it expects memory ordering to be
enforced — which bears directly on gap 1.3. And the `gx2` entries are GPU state
(`GX2SetLineWidth`, `GX2SetAlphaToMaskReg`) that currently does nothing, so they are candidate
causes for any visual difference in this title.

The audio surface being the least complete thing BotW actually touches was not predicted by any
prior analysis in this repo.

### 4.3 HLE function names are registered and discarded

`PPCInterpreter_registerHLECall(HLECALL hleCall, std::string hleName)` never uses `hleName`
`[SRC PPCInterpreterHLE.cpp:25-45]`. Names arrive fully qualified as `"lib.func"`. Storing them into
a parallel array costs one startup-time allocation each and nothing at runtime, and it is what makes
a *named* HLE histogram possible.

### 4.4 Stats overlay gate omits two of its own options

```cpp
if (config.overlay.fps || config.overlay.drawcalls || config.overlay.cpu_usage
    || config.overlay.cpu_per_core_usage || config.overlay.ram_usage)
```
`[SRC LatteOverlay.cpp:81]` — `vram_usage` and `debug` are missing, so an overlay configured with
only `debug` enabled renders **nothing**. One-line fix, and it blocks using the overlay to
cross-check telemetry values.

### 4.5 Dead Vulkan references in a Metal-only fork

`GetConfig().vk_accurate_barriers` is still read in the Metal draw path
`[SRC MetalRenderer.cpp:1147]` and `InfoLog_PrintActiveSettings()` still has a
`GraphicAPI::kVulkan` branch `[SRC CafeSystem.cpp:247-253]`. Cruft, not a divergence, but it makes
the settings dump misleading.

---

## Claims previously recorded as gaps that are **not** gaps

Kept here explicitly so they are not re-raised. Both were inherited from
`../porting/03-graphics-metal.md` and do not survive inspection on this fork's target hardware.

| Claim | Reality |
|---|---|
| "**RECTS** draws are silently dropped" | **False.** `RECTS` maps to `MTL::PrimitiveTypeTriangle` and is emulated as two triangles `[SRC LatteToMtl.cpp:254-255]`, with culling handled at `[SRC MetalRenderer.cpp:1327-1329]`. Approximated, not absent. |
| "**Geometry-shader** draws are silently dropped" | **Dead branch on our hardware.** The drop is `if (usesGeometryShader && !m_supportsMeshShaders) return;` `[SRC MetalRenderer.cpp:1170]`, and `m_supportsMeshShaders = m_supportsMetal3 && (vendor != Intel || force)` `[SRC :175]`. On Apple Silicon both conditions hold, so the branch is never taken. It *is* a genuinely silent return — no log, unlike every other early-out in that function — so it is worth adding a signal, but it is not currently losing draws. |

---

## Priority for the telemetry harness

Ordered by "what would we learn":

1. **4.2 + 4.3** — named unresolved-import and HLE-call reporting. Cheap, and it is the only way to
   discover *unknown* gaps rather than re-counting known ones.
2. **1.2** — self-dependency detection in counting-only mode. Tells us the real cost of the fix
   before paying for it.
3. **1.1** — `D24_S8` usage count. Tells us which titles are affected.
4. **2.5** — audio frames serviced per video frame. Tests a specific hypothesis.
5. **1.3** — `DCStoreRange` overlap count.
6. **2.4** — HLE call histogram, which 4.3 enables anyway.

Rows 4.1, 4.4 and 4.5 are fixes, not measurements, and should simply be done.
