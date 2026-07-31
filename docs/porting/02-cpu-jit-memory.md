<!-- Generated during the Apple Silicon / macOS 26 fork research pass.
     Findings verified against the source tree at commit b8f2cf4 and the
     macOS 26.5.2 / Xcode 26.6 SDK on an Apple M2. See 00-master-plan.md. -->

# CPU / JIT / MEMORY workstream — TesseraEmu arm64 hard fork

## Verification notes from my own read

A few of the briefed claims I confirmed, corrected, or refined — these change the plan:

- **`FPR_t` is already `{double fp0; double fp1;}`** (`src/Cafe/HW/Espresso/PPCState.h:14-30`), 16 bytes contiguous. A single `ldr q` / `str q` does load a whole guest FPR. **And the backend already does this**: `r_name`/`name_r` for `PPCREC_NAME_TEMPORARY_FPR0` use `fpReg<QReg>` (`BackendAArch64.cpp:448`, `:529`). So *F64 vregs that are secretly 128-bit already exist in this codebase.* This is the single most important fact for item 5 and it collapses the cost estimate dramatically.
- **`PPCREC_IML_TYPE_FPR_R_R_R_R` already exists** with a 4-reg union, correct `CheckRegisterUsage` (3 reads + 1 write, `IMLInstruction.cpp:296-303`), correct `RewriteGPR` (`:515-521`), and a working `IMLDebug` printer (`IMLDebug.cpp:446-450`). FMA needs **no new IML instruction type** — only four new `operation` values.
- **`IMLRegisterAllocatorParameters::perTypePhysPool` is indexed per `IMLRegFormat`** (`IMLRegisterAllocator.h:114-122`), so GPR and FPR pools are separate `IMLPhysRegisterSet` objects. The "only 8 bits of headroom" concern is real *only* for the mixed `volatileRegs` set built in `GetInstructionFixedRegisters` (`IMLRegisterAllocator.cpp:139-146`, max index 56). Don't widen it; don't add a new `IMLRegFormat`.
- **Cycle *checks* are only emitted on backward branches** — `PPCRecompiler_HandleCycleCheckCount` (`PPCRecompilerImlGen.cpp:3047-3053`) returns early unless `branchTarget <= startAddress`. Cycle *decrements* are per basic block (`:3163-3168`). This asymmetry is what makes item 7 cheap.
- **`PPCTimer` ratios are exact rationals.** `CORE_CLOCK/cntfrq = 1243125000/24000000 = 3315/64` exactly. Guest timebase `= 663/256` exactly. No 128-bit math is needed at all.
- **`temporaryGPR[4]` is referenced only by `BackendX64.cpp:835-893`.** Safe to delete with the purge. `PPCInterpreter_t` is never serialized (only `malloc`+`memset`, `PPCInterpreterMain.cpp:20-21`) so the layout shift is contained by the `static_assert`s at `BackendAArch64.cpp:366-378` (13 when this was written, 14 today).
- **`x28` / `MEM_BASE_REG` and `x27` are only live inside JIT code** — `enterRecompilerCode` sets them on entry (`:1655-1657`) and they're callee-saved across the whole JIT region. Relevant to the fiber-switch register list (R5).
- Claim I could **not** verify (submodule not checked out): whether xbyak_aarch64's `add_imm` / `sub_imm` / `cmp_imm` helpers already select `add #imm12` / `add #imm12, lsl 12` before falling back to `mov`+reg. **Verify this before sizing item 3c** — if they do, `compare_s32` is already fine and only the AND/OR/XOR/MUL paths need work.

---

# Phase 0 — Measurement and correctness harness (do this first; nothing else is claimable without it)

## 0.1 Static codegen metric — the primary signal

Frame-rate benchmarking on a 3-core-fiber emulator with a GPU in the loop, on a 4P+4E machine with 8 GB, is not going to give you attributable deltas for a 1-instruction codegen change. The primary metric must be static and deterministic.

**Build a `--jit-audit` mode.** After RPL load, before the title runs: walk `MEMORY_CODEAREA_ADDR..+SIZE`, use `PPCFunctionBoundaryTracker` to enumerate function ranges, force-compile every one synchronously (the `PPCREC_FORCE_SYNCHRONOUS_COMPILATION` path in `PPCRecompiler.cpp:69-88` already exists), and emit a CSV per function:

```
ppcAddr, ppcInstrCount, imlCount_preRA, imlCount_postRA, imlNoOpCount,
spillCount, fillCount, hostBytes, hostInstrCount, compileTimeNs
```

Then the headline number is **`hostInstrCount / ppcInstrCount`** summed over the title. It is deterministic, diffable across commits, and every item in Phase 2 and 3 moves it.

Instrumentation points:
- `PPCRecompiler_recompileFunction` (`PPCRecompiler.cpp:198-283`) — `BenchmarkTimer bt` already exists behind `PPCREC_LOG_RECOMPILATION_RESULTS` (`:24`, `:202-205`, `:273-281`). Promote it to a runtime flag.
- `hostBytes` = `PPCRecFunction_t::x86Size` — **broken today**, see 1.3. Fix that first or the metric is garbage.
- `hostInstrCount` = `hostBytes / 4` (fixed-width, trivially exact once `x86Size` is right).
- `spillCount`/`fillCount`: count `PPCREC_IML_TYPE_R_NAME` / `NAME_R` after `PPCRecompiler_NativeRegisterAllocatorPass`.

**Also emit two histograms** over the whole title: PPC opcode frequency (static, and weighted by basic-block reachability) and IML opcode frequency post-RA. You need these to know that e.g. `lhz` is 4% of static instructions *before* spending a day on `rev16`. Effort **M**. This is the highest-leverage item in the plan.

## 0.2 Dynamic profiling on Apple Silicon

`xctrace record --template 'CPU Counters' --launch ./Cemu` with the counter set {`INST_RETIRED`, `CORE_ACTIVE_CYCLE`, `BRANCH_MISPRED_NONSPEC`, `L1D_CACHE_MISS_LD`, `MAP_STALL`}. The hard part is that JIT frames have no symbols and the sampler will bottom out at `PPCRecompiler_enterRecompilerCode`.

Fix: emit a **JIT symbol map** side file. You already have exactly the data — `PPCRecFunction_t{ x86Code, x86Size, ppcAddress }` and `jumpTableEntries`. Write `~/Library/Logs/Cemu/jitmap-<pid>.txt` as `hostAddr hostSize ppcAddr` lines, appended under `recompilerSpinlock` in `PPCRecompiler_makeRecompiledFunctionActive`. Post-process `xctrace export --xpath '//trace-toc/.../table[@schema="time-profile"]'` sample addresses through the map. Log the JIT arena base range at startup so you can filter.

Validate the map *after* item 4.2 (fiber switch) — a hand-written context switch is exactly what silently breaks Instruments' unwinder, and you will not notice until your profiles are meaningless.

## 0.3 Differential correctness harness — gate for items 3.2 and 5.3

You cannot touch FP semantics on a fork with no upstream to diff against without this.

Build a standalone test binary linking the recompiler + interpreter (both are callable in isolation: `PPCRecompiler_generateAArch64Code(PPCRecFunction_t*, ppcImlGenContext_t*)` and `PPCInterpreterSlim_executeInstruction`). For each PPC opcode of interest:
1. Synthesize a 1-4 instruction guest function.
2. Randomize `PPCInterpreter_t` (gpr, fpr, cr, xer, fpscr) from a corpus weighted toward: denormals, ±0, ±inf, sNaN, qNaN, 2^±1022, values near single-precision boundaries.
3. Run interpreter and JIT on identical copies.
4. `memcmp` the full `PPCInterpreter_t` minus `remainingCycles`/`instructionPointer`, plus a memory window.

Run 10^7 iterations per opcode in CI. This is the only mechanism that makes item 3.2 (FMA) safe to ship.

**Cheap partial substitute available today:** `LaunchSettings::GetPPCRecLowerAddr/UpperAddr` (`LaunchSettings.h:41`, consumed at `PPCRecompiler.cpp:216-227`) already lets you restrict recompilation to an address range — that is a ready-made binary-search bisector for "which function did my codegen change break". Document it; you will use it constantly.

---

# Phase 1 — Subtraction (zero risk, unblocks everything)

## 1.1 The x86 purge — sequencing

Nothing has to be replaced first. The AArch64 path is complete and self-sufficient; this is pure subtraction and it makes every later diff readable. Order:

1. `src/Cafe/CMakeLists.txt:86-94` — remove the 9 `BackendX64/*` entries. Delete the directory and `x86Emitter.h`.
2. Remove `#include "BackendX64/BackendX64.h"` from `PPCRecompiler.cpp:17` and `#include "../BackendX64/BackendX64.h"` from `IMLOptimizer.cpp:7`. **These are the only two includes** — verified.
3. `IMLOptimizer.cpp`: delete `IMLOptimizerX86_SubstituteCJumpForEflagsJump` (`:622-700`), `IMLOptimizerX86_ModifiesEFlags`, and the `#ifdef ARCH_X86_64` block in `IMLOptimizer_StandardOptimizationPassForSegment` (`:703-710`).
   **Keep `IMLUtil_FindInstructionWhichWritesRegister` (`:449`), `IMLUtil_CanMoveInstructionTo` (`:465`), `IMLUtil_CountRegisterReadsInRange` (`:509`), `IMLUtil_MoveInstructionTo` (`:529`)** — these are the substrate for the AArch64 peephole pass in 5.4.
4. `IMLInstruction.h`: delete `PPCREC_IML_OP_X86_CMP` (`:161`), `PPCREC_IML_TYPE_X86_EFLAGS_JCC` (`:245`), the `op_x86_eflags_jcc` union member (`:494-499`), `make_x86_eflags_jcc` (`:806-812`), and the `X86_EFLAGS_JCC` term in `IsSuffixInstruction` (`:519`). `IMLInstruction.cpp`: the handlers at `:528-531` and the equivalent in `CheckRegisterUsage`. `IMLDebug.cpp:461-464`.
5. `IMLRegisterAllocator.cpp:126-179`: delete the `#else` (x86) half of `GetInstructionFixedRegisters`, keep `SetupCallingConvention` (shared) and drop the `#if defined(__aarch64__)` guard.
6. `IMLInstruction.h:822-826`: delete `namespace IMLArchX86`.
7. `PPCRecompiler.h:134-150`: delete the `_x64XMM_*` fields and `_x64XMM_mxCsr_*`. `ppcRecompilerDirectJumpTable` is field 0 and the backend addresses it by `offsetof`, so this is safe. Delete `PPCRecompiler_initPlatform`'s x86 body (`PPCRecompiler.cpp:611+`).
8. `PPCState.h:70`: delete `uint32 temporaryGPR[4]`. Only `BackendX64.cpp:835-893` referenced it. This shifts every subsequent `PPCInterpreter_t` offset — which is exactly what `BackendAArch64.cpp:366-378` guards. **The static_asserts will re-verify automatically at compile time.** No serializer touches the struct.
9. `PPCRecompiler.cpp:112-130`: the `#if BOOST_OS_WINDOWS` `_controlfp` block in `PPCRecompiler_enter` — delete. Do **not** replace it with FPCR manipulation (see R4).
10. Delete `PPCRecompiler_optimizePSQLoadAndStore` declaration at `IML.h:9` — never defined, never called.
11. `ARCH_X86_64` blocks in: `precompiled.h`, `cpu_features.cpp` (also delete the dead `x86.movbe` detection), `PPCTimer.cpp` (superseded by 4.1), `aes128.cpp` (superseded by 5.1), `LatteIndices.cpp`, `ExceptionHandler_posix.cpp`, `GDBBreakpoints.{h,cpp}`, `coreinit_Thread.cpp:30-40`.
12. **Delete the LL/SC atomic fallback.** `BackendAArch64.cpp:1116-1140` branches on `s_cpu.isAtomicSupported()`. FEAT_LSE is architectural on every Apple Silicon core (M1 is ARMv8.4+). Hardcode the `casal` path, delete `static const util::Cpu s_cpu` (`:61`) and the `ldaxr`/`stlxr` loop. Saves a runtime branch at compile time and ~15 lines.
13. `ATTR_MS_ABI` (`precompiled.h:593-595`) expands to nothing on non-Windows. Strip it from `PPCRecompiler_enterRecompilerCode`, `leaveRecompilerCode_*`, `PPCREC_JUMP_ENTRY`, `frsqrte_espresso`, `fres_espresso`.

**Effort L (mechanical). Verification:** build green + `--jit-audit` CSV byte-identical to pre-purge.

## 1.2 `betype.h` — add `__builtin_bswap`

`src/Common/betype.h:16-20`. The generic fold is pattern-matched by clang at `-O2` into `rev`, but not at `-O0`/`-O1`. Since you are about to spend months running this under lldb and Instruments, debug-build speed of *every guest memory access* matters.

```cpp
template<class T, class U = std::make_unsigned_t<T>>
constexpr T bswap(T i)
{
    if (!std::is_constant_evaluated()) {
        if constexpr (sizeof(T) == 2) return (T)__builtin_bswap16((U)i);
        else if constexpr (sizeof(T) == 4) return (T)__builtin_bswap32((U)i);
        else if constexpr (sizeof(T) == 8) return (T)__builtin_bswap64((U)i);
    }
    return (T)bswap_impl<U>((U)i, std::make_index_sequence<sizeof(T)>{});
}
```

Same file, `SwapEndian` (`:36-44`) does `*(uint32*)&value` on floats — UB and an aliasing barrier. Replace with `std::bit_cast`. Effort **S**. Verification: debug-build `--jit-audit` wall time.

## 1.3 JIT size/cursor bugs — 3 lines, do before 0.1 lands

Two coupled bugs in `PPCRecompiler_generateAArch64Code`:

- `processAllJumps()` (`BackendAArch64.cpp:1601`) calls `setSize(jumpStart)` inside the `std::visit` (`:181`) and never restores the cursor, so after it returns `getSize()` points at the last patched jump.
- `:1616` uses `getMaxSize()` (AutoGrow buffer *capacity*, typically 2× the real size) instead of `getSize()`.

Fix:
```cpp
size_t codeEnd = aarch64GenContext.getSize();
if (!aarch64GenContext.processAllJumps()) { ... }
aarch64GenContext.setSize(codeEnd);
aarch64GenContext.readyRE();
PPCRecFunction->x86Size = aarch64GenContext.getSize();
```

**Flagged hazard (R8):** `getMaxSize()` being *larger* is currently masking a potential under-flush. `readyRE()` in AutoGrow mode flushes based on the emitter's notion of size. After this fix, verify what range `xbyak_aarch64::CodeGenerator::readyRE()` passes to the I-cache flush, and if there is any doubt add an explicit `sys_icache_invalidate(code, size)` after finalize. A too-small flush produces stale-icache crashes only on cold paths, only on some cores — the worst possible failure signature.

Effort **S**. Verification: the recompiler dump path (`PPCRecompiler.cpp:250-258`) should now produce disassemblable output end-to-end (`llvm-objdump -d --triple=arm64 -b binary`); today it writes trailing garbage.

---

# Phase 2 — Codegen wins, ordered by payoff/effort

All measurable via 0.1. None of these change semantics.

## 2.1 Offset-0 addressing-mode fold — **highest instruction-count win in the plan**

Sites (all identical shape): `BackendAArch64.cpp:1013` (`load`), `:1069` (`store`), `:1154`/`:1174` (`fpr_load`), `:1199`/`:1218`/`:1227` (`fpr_store`).

```cpp
// current, unconditional:
add_imm(TEMP_GPR1.WReg, memReg, memOffset, TEMP_GPR1.WReg);
if (indexed) add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, idxReg);
auto adr = AdrExt(MEM_BASE_REG, TEMP_GPR1.WReg, ExtMod::UXTW);
```

Replacement:
```cpp
WReg addrReg;
if (memOffset == 0 && !indexed)        addrReg = memReg;                        // 0 insns
else if (memOffset == 0 && indexed)  { add(TEMP_GPR1.WReg, memReg, idxReg);      addrReg = TEMP_GPR1.WReg; }
else                                 { add_imm(TEMP_GPR1.WReg, memReg, memOffset, TEMP_GPR1.WReg);
                                       if (indexed) add(TEMP_GPR1.WReg, TEMP_GPR1.WReg, idxReg);
                                       addrReg = TEMP_GPR1.WReg; }
auto adr = AdrExt(MEM_BASE_REG, addrReg, ExtMod::UXTW);
```

Correctness: guest EA is `mod 2^32` and `UXTW` zero-extends the 32-bit index, identical to feeding the sum through a W register. Every `lwzx`/`stwx`/`lfsx`/`stfsx` (all indexed forms use `immS32 == 0`, see `make_fpr_r_memory_indexed`, `IMLInstruction.h:719`) and every `lwz r,0(rA)` benefits.

**Effort S. Payoff: 1 instruction off a large fraction of all guest memory ops.** Verification: `--jit-audit` hostInstr/ppcInstr delta; expect a visible drop.

## 2.2 FP load/store via NEON byte-reverse — kills a cross-domain move on every FP memory op

Current single-float load (`:1155-1163`) is `ldr w26; rev w26; fmov s,w26; fcvt d,s` — four instructions *and* a GPR→FPR transfer, which on Apple cores is ~5-6 cycle latency and blocks in the rename/dispatch path. Guest FP loads are pervasive in Wii U geometry code.

Replacements (`TEMP_FPR` = v31 is available as scratch):

| op | now | replacement | Δ |
|---|---|---|---|
| f32 load, swapped, expand | `ldr w; rev w; fmov s,w; fcvt d,s` | `ldr(dataSReg, adr); rev32(dataVReg.b8, dataVReg.b8); fcvt(dataDReg, dataSReg)` | 4→3, no GPR |
| f32 load, `notExpanded` | `ldr w; rev w; fmov s,w` | `ldr(dataSReg, adr); rev32(dataVReg.b8, dataVReg.b8)` | 3→2, no GPR |
| f64 load | `ldr x; rev x; fmov d,x` | `ldr(dataDReg, adr); rev64(dataVReg.b8, dataVReg.b8)` | 3→2, no GPR |
| f32 store | `fcvt s,d; fmov w,s; rev w; str w` | `fcvt(TEMP_FPR.SReg, dataDReg); rev32(TEMP_FPR.b8, TEMP_FPR.b8); str(TEMP_FPR.SReg, adr)` | 4→3, no GPR |
| f64 store | `fmov x,d; rev x; str x` | `rev64(TEMP_FPR.b8, dataVReg.b8); str(TEMP_FPR.DReg, adr)` | 3→2, no GPR |
| `ST_MODE_UI32_FROM_PS0` | `fmov w,s; rev w; str w` | `rev32(TEMP_FPR.b8, dataVReg.b8); str(TEMP_FPR.SReg, adr)` | 3→2, no GPR |

`REV32 Vd.8B, Vn.8B` reverses bytes within each 32-bit element of the low 64 bits and zeroes the upper 64 — exactly right. `REV64 Vd.8B` reverses all 8 bytes.

**Effort S-M. Payoff: high** — removes both an instruction and a domain-crossing stall from every FP memory access. Also composes directly with 5.2. Verification: 0.3 harness on `lfs/lfd/stfs/stfd/lfsx/stfsx/psq_l/psq_st`, plus `--jit-audit`.

## 2.3 `rev16` for 16-bit endian swaps

Load (`:1024-1041`): unsigned path `ldrh; rev; lsr #16` → `ldrh; rev16` (**3→2**). Signed path (`lha`) stays 3 (`ldrh; rev16; sxth`) — no win, but no loss either, and it's clearer.
Store (`:1085-1093`): `rev; lsr #16; strh` → `rev16; strh` (**3→2**).

Wins on `lhz`, `lhzx`, `lhbrx`, `sth`, `sthx`, `sthbrx`. Effort **S**. Check the 0.1 opcode histogram first to size this — I expect 3-6% of static instructions.

## 2.4 AArch64 immediate encodings

**Verify xbyak's `add_imm`/`sub_imm`/`cmp_imm` first.** If they already select `#imm12` and `#imm12,lsl 12`, then `compare_s32` (`:839`) and the ADD/SUB paths in `r_r_s32` (`:611-618`) are already fine, and only the following need work:

- **AND/OR/XOR** (`:617-636`): currently unconditional `mov(TEMP_GPR1.W, imm); and_/orr/eor`. AArch64 logical-immediate encoding (N:immr:imms) covers every value that is a rotated run of ones with a power-of-2 repeat period. That is *exactly* the shape of `andi./ori/xori/andis./oris/xoris` immediates and of `rlwinm` mask lowering — a very high hit rate. Saves 1-2 instructions each.
  Write your own predicate `bool isLogicalImm32(uint32 v)` (standard ~30-line algorithm: reject 0 and ~0, find the run period, check it's a rotated contiguous run). **Do not rely on xbyak throwing** — test the predicate exhaustively: there are only **5334 distinct encodable 32-bit values**, so enumerate all of them plus 10^7 random rejects and assert `predicate(v) ⟺ xbyak encodes v and decodes back to v` (R10).
- **`MULTIPLY_SIGNED` with immediate** (`:635`): strength-reduce `mulli` by 2^k → `lsl`; by 2^k±1 → `add/sub Wd, Wn, Wn, lsl #k`. Struct-size array indexing makes this common.

Effort **S-M** (mostly the predicate + its test).

## 2.5 `ldp`/`stp` fusion for name spill/fill — do it in the backend, not in IML

The register allocator emits runs of consecutive `R_NAME` / `NAME_R` at segment boundaries. `gpr[]` is contiguous 4-byte, `cr[]` contiguous 1-byte, and **`fpr[N].ps0` / `fpr[N].ps1` are adjacent doubles** — that last one is the big one, because the FPU IR gen touches both halves of a guest FPR constantly.

**Recommendation: implement this as a peek-ahead in the emit loop of `PPCRecompiler_generateAArch64Code` (`:1450-1470`), not as an IML pass.** That way you don't touch IML types, DCE, RA, or the debug printer. ~60 lines.

Algorithm: on hitting `PPCREC_IML_TYPE_R_NAME`, collect the maximal run of consecutive `R_NAME` instructions; sort by resolved `PPCInterpreter_t` byte offset (legal — RA emits one per distinct vreg, all destinations distinct, all independent loads); greedily pair adjacent-offset entries of matching width into `ldp`. Same for `NAME_R`/`stp`. Constraints: `ldp W` scaled imm7 range is ±252 bytes (`gpr` spans offsets 4..131 — all in range); `ldp D` is ±504 (`fpr` spans 136..647 — the tail needs a base adjust, or just skip pairs out of range).

**Effort M. Payoff: real on hot loop segments** — a segment with 6 GPR fills drops 3 instructions per iteration. Verification: `--jit-audit` spill/fill count vs hostInstrCount.

---

# Phase 3 — The two structural wins

## 3.1 Cycle counting as a first-class IML register — **largest single win for loop-heavy code**

Today, `PPCREC_IML_MACRO_COUNT_CYCLES` (`BackendAArch64.cpp:939-947`) is `ldr` + `sub_imm` + `str` against `hCPU->remainingCycles` **on every basic block**, and `CJUMP_CYCLE_CHECK` (`:858-865`) adds an `ldr` on every loop back-edge. That's a load-store round trip through L1 with a serialising dependency, per block. Also `sub_imm(..., TEMP_GPR2)` clobbers x26 — **which is the guest-IP carrier (`LR`)** — a latent aliasing hazard.

**Approach: make `remainingCycles` an IML name and let the existing register allocator pin it.** Do not hand-reserve a physical register.

1. `IMLInstruction.h:264` — add `PPCREC_NAME_REMAINING_CYCLES = 6003` (adjacent to the XER block).
2. `PPCRecompilerImlGen.cpp:3163-3168` — replace the `MACRO_COUNT_CYCLES` insertion with
   `make_r_r_s32(PPCREC_IML_OP_SUB, regCycles, regCycles, ppcInstructionCount)`
   where `regCycles = PPCRecompilerImlGen_LookupReg(ctx, PPCREC_NAME_REMAINING_CYCLES, IMLRegFormat::I32)`.
3. Give `PPCREC_IML_TYPE_CJUMP_CYCLE_CHECK` a register operand (add to `op_conditional_jump` or a small dedicated struct) and add it to `CheckRegisterUsage` (`IMLInstruction.cpp:136`) as a **read** of `regCycles`. Keep the type distinct so you keep the single `tbnz #31` and never materialise a bool.
4. Backend: add the `PPCREC_NAME_REMAINING_CYCLES` case to `r_name`/`name_r` (`:380`,`:461`) at `offsetof(PPCInterpreter_t, remainingCycles)`, plus a `static_assert(isAdrImmValidGPR(...))` alongside the existing 13. Delete the `MACRO_COUNT_CYCLES` case. `conditionalJumpCycleCheck` becomes just `prepareJump(NegativeRegValueJumpInfo{ .regValue = gpReg<WReg>(inst.regCycles) })` — **the `ldr` disappears too**.

**Why this is correct without extra work:** every construct that mutates `remainingCycles` from C — `PPCRecompiler_virtualHLE` (`:927`), `PPCInterpreter_relinquishTimeslice`, `PPCCore_boostQuantum/deboostQuantum` (`PPCScheduler.cpp:12-32`) — is reached only through `MACRO_HLE`, `MACRO_BL`, `MACRO_B_FAR`, `MACRO_B_TO_REG`, or `MACRO_LEAVE`. All five are suffix instructions on segments with `nextSegmentIsUncertain = true` (`PPCRecompilerImlGen.cpp:3091-3096`), and `IMLOptimizerRegIOAnalysis::ComputeDepedencies` imports *all* registers at such segments (`IMLOptimizer.cpp:317-327`). So the RA already writes every live name back before them. The `ldr` inside `MACRO_HLE` (`:972`) reads memory the C call just wrote — still correct.

Net: within a function, N basic blocks cost 1 `ldr` at entry + N `sub` + 1 `str` per exit, instead of 3N + a check-load per back-edge. On a 3-block loop that's ~9 instructions/iteration → ~1.

**Effort M. Risk: R2 — this is the highest-risk item in the plan** because a missed write-back manifests as a *scheduling* divergence: nondeterministic hangs that reproduce once in 100 boots. Mitigation: in debug builds keep a shadow `remainingCycles` in `PPCInterpreter_t`, updated by an extra `NAME_R` after every `SUB`, and assert equality at every `leaveRecompilerCode` and at entry to `PPCRecompiler_virtualHLE`.

**Rejected alternative:** reducing accounting frequency (count only at back-edges and exits). It changes scheduling granularity; with quantum = 45000 (`PPCScheduler.cpp:10`) the drift is bounded but visible in timing-sensitive titles. Don't.

## 3.2 FMA — a **correctness fix** that also removes instructions

### Accuracy: confirmed, this is strictly more correct

PPC `fmadd` computes `round(a·c + b)` with a *single* rounding of the infinitely-precise product-sum. The current lowering (`PPCRecompilerImlGenFPU.cpp:283-292`, `:495-521`) computes `round(round(a·c) + b)` — a double rounding. AArch64 `FMADD` is single-rounding. **Emitting `fmadd` is the architecturally correct behaviour and the current code is wrong.** For `fmadds`, PPC does the fused op at double precision then rounds to single, so `fmadd d; fcvt s,d; fcvt d,s` (the existing `roundToSinglePrecision`) is exact.

### The trap: PPC↔ARM mnemonic crossover

ARM A64 semantics (`Da` is the addend):
- `FMADD  Dd,Dn,Dm,Da` → `Da + Dn·Dm`
- `FMSUB  Dd,Dn,Dm,Da` → `Da − Dn·Dm`
- `FNMADD Dd,Dn,Dm,Da` → `−Da − Dn·Dm`
- `FNMSUB Dd,Dn,Dm,Da` → `−Da + Dn·Dm`

PPC:
| PPC | semantics | **ARM instruction** |
|---|---|---|
| `fmadd` | `A·C + B` | `fmadd(D, A, C, B)` |
| `fmsub` | `A·C − B` | **`fnmsub`**`(D, A, C, B)` |
| `fnmadd` | `−(A·C + B)` | `fnmadd(D, A, C, B)` |
| `fnmsub` | `−(A·C − B)` | **`fmsub`**`(D, A, C, B)` |

`fmsub`↔`fnmsub` are **swapped**. This is the #1 way to ship a subtly broken FMA. Unit-test all four explicitly.

### IR design — no new instruction type needed

Reuse `PPCREC_IML_TYPE_FPR_R_R_R_R`. Add four `operation` values to `IMLInstruction.h` (after `PPCREC_IML_OP_FPR_SELECT`, `:140`), defined in **PPC operand order** so the ISA lowering is trivial and the mnemonic crossover lives in exactly one place:

```
PPCREC_IML_OP_FPR_FMADD,   // regR = regA*regB + regC
PPCREC_IML_OP_FPR_FMSUB,   // regR = regA*regB - regC
PPCREC_IML_OP_FPR_FNMADD,  // regR = -(regA*regB + regC)
PPCREC_IML_OP_FPR_FNMSUB,  // regR = -(regA*regB - regC)
```

- `CheckRegisterUsage` (`IMLInstruction.cpp:296-303`): **already correct** — regA/B/C read, regR written, regR *not* read. That is exactly FMA's non-destructive signature.
- `RewriteGPR` (`:515-521`): already correct.
- `IMLDebug_DisassembleInstruction` (`IMLDebug.cpp:446-450`): already correct; add names to `IMLDebug_GetOpcodeName`.
- Backend `fpr_r_r_r_r` (`BackendAArch64.cpp:1341-1358`): add four cases with the crossover table above.

### Register-allocator implications: pressure goes *down*

AArch64 FMA is fully non-destructive (`Dd`, `Dn`, `Dm`, `Da` may all differ or alias freely). The current lowering exists almost entirely to work around x86's destructive 2-operand form — hence all the `frB == frD` / `frD == frC` special cases and the `DefineTempFPR(fprTemp, 0)` traffic through `PPCInterpreter_t::temporaryFPR[]`. **All of that disappears.** The RA already handles a 3-read/1-write FPR instruction (`FPR_SELECT`). Net: fewer live ranges, fewer `R_NAME`/`NAME_R` against `temporaryFPR`.

### Sites

Scalar (`PPCRecompilerImlGenFPU.cpp`): `_FMADD` (`:268`), `_FMSUB` (`:305`), `_FNMSUB` (`:339`), `_FMADDS` (`:495`), `_FMSUBS` (`:523`), `_FNMSUBS` (`:549`). Each collapses to one `make_fpr_r_r_r_r(...)` plus, for the `*S` variants, `PPRecompilerImmGen_roundToSinglePrecision` + `PSE_CopyResultToPs1()`.

**Free bug fix:** `PPCRecompilerImlGen_FMSUB` at `:317` does `return false` in the `frB == frD` branch — that silently aborts recompilation of the *entire enclosing function*, dropping it back to the interpreter. Gone.

Paired (do in the same change — this is where the instruction-count win is): `PS_MADDSX` (`:1046`), `PS_MADD` (`:1233`), `PS_NMADD` (`:1301`), `PS_MSUB`/`PS_NMSUB` (`:1356`). Each currently emits 6-10 IML instructions with two `temporaryFPR` round trips; each becomes **2** FMA instructions. `PS_MADD` alone goes 10 → 2.

**Effort M. Gate on 0.3.** Ship behind a `GameProfile` toggle (default ON) as a release valve for R1.

**Do not** attempt the `frC` 25-bit rounding that real Espresso performs (the `// todo - round fprC to 25bit` comments at `:1027`, `:1067`). It's orthogonal, it's a behaviour change, and it costs instructions.

---

# Phase 4 — Infrastructure

## 4.1 `PPCTimer.cpp` rewrite

### Is 24 MHz `cntvct_el0` adequate?

The guest timebase (`coreinit_GetMFTB` = `PPCInterpreter_getMainCoreCycleCounter()/20`, `coreinit_Time.cpp:6-10`) runs at `TIMER_CLOCK` = 248625000/4 = **62.15625 MHz** = 16.09 ns/tick. Host `cntvct_el0` is 24 MHz = 41.67 ns. **The host clock is 2.6× coarser than the guest timebase.** So the guest timebase advances in jumps of ~2.6 ticks.

Is that a problem? Two cases:
- Wall-clock / interval use (`OSTicksToMilliseconds`, alarms, `OSSleepTicks`): trivially fine.
- Guest code that polls `mftb` in a tight loop and requires strict increase between consecutive reads: with 41.67 ns granularity and ≥30 guest instructions between reads (each ~1-3 host ns under JIT), consecutive reads *usually* differ but not always.

Crucially, **the current implementation already has exactly this resolution** — it's the same `cntvct_el0` behind the `__rdtsc()` shim (`precompiled.h:363-368`). So this is not a regression. But it's cheap to improve: add a monotonic bump so two reads never return the same value. Do that.

There is no better clock available. `mach_absolute_time()` and `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` are both `cntvct_el0` scaled by `mach_timebase_info` (125/3 → 41.667 ns) plus function-call overhead. **Use `cntvct_el0` directly.**

### Replacement math — exact rationals, no 128-bit anything

```
CORE_CLOCK / cntfrq = 1243125000 / 24000000 = 3315/64   (exact)
guest timebase      =   62156250 / 24000000 =  663/256  (exact)
```

So `PPCTimer_getFromRDTSC()` becomes, in full:

```cpp
uint64 PPCTimer_getFromRDTSC()
{
    uint64 vct = read_cntvct();                       // mrs x, cntvct_el0
    uint64 delta = vct - s_baseVct;                   // both from the rebase snapshot
    // (delta * 3315 >> 6) << 3 >> shift   folded:
    return s_baseTicks + ((delta * (3315ull << 3)) >> (6 + s_timerShift));
}
```

Overflow: `delta * 26520` overflows after `2^64/26520` ticks = 2.3×10^7 s of *uptime since rebase* ≈ 27 years. Safe.

**Delete outright:** `sTimerSpinlock` (`:124`), `_rdtscAcc`, `_rdtscLastMeasure`, `uint128_t`, `muldiv64`, `PPCTimer_estimateRDTSCFrequency` (`:34-70`, the **3-second startup stall**), `PPCTimer_initThread`, the detached thread in `PPCTimer_init`, `PPCTimer_waitForInit`, and the `_umul128`/`_udiv128` uses (`:106-109`, `:137`, `:153`). `PPCTimer_isReady()` returns `true`.

`PPCTimer_microsecondsToTsc(us)` = `us * (cntfrq/1000000)` = `us * 24`. `PPCTimer_tscToMicroseconds(t)` = `t / 24`. Compute the rational generically at init (read `cntfrq_el0`, reduce by gcd) so a future chip with a different frequency doesn't silently break; assert it equals 24000000 and log if not.

**The one piece of state you must keep:** `ActiveSettings::GetTimerShiftFactor()` can change at runtime. A pure function of `cntvct` means changing the shift retroactively rescales the *entire history* → the guest clock jumps. Fix: on shift change, snapshot `{s_baseVct, s_baseTicks}` under a mutex (rare path) and read both under a seqlock on the hot path (or, simplest and adequate: snapshot the setting once at title boot and ignore mid-session changes).

Also fix the `__rdtsc()` shim (`precompiled.h:363-368`) — it should be `isb; mrs cntvct_el0` if you need ordering, but for a timestamp read you don't; and `_mm_mfence` (`:370-374`) mapping to a `seq_cst` fence should not be called from timer code at all after this rewrite.

**Effort S-M. Payoff:** removes a 3-second boot stall, and removes a global spinlock + `dmb ish` + 128-bit divide from a function called on every guest `mftb`, `OSGetSystemTime`, alarm poll, and scheduler tick. Today all three guest cores serialise through one spinlock to read the clock. **Verification (R3):** log `OSGetSystemTime()` deltas vs `CLOCK_MONOTONIC` over 60 s, fail above 100 ppm drift; plus stopwatch an in-game timer.

## 4.2 Fiber switch — hand-written AArch64

`swapcontext` on Darwin/arm64 saves the full callee-saved set **and calls `sigprocmask`** (a syscall, ~200-500 ns) on both save and restore. This is on the critical path of every guest thread switch.

### Measured, 2026-07-30 — `tools/probes/fiber_switch_cost.c`

The ~600-700 ns figure below was an estimate and is now replaced by a ping-pong microbenchmark (10⁶ switches, median of 5 interleaved runs):

| | ns/switch |
|---|---|
| `swapcontext` (what `FiberUnix.cpp` does today) | **454.5** (range 454.1 – 474.4) |
| hand-written AArch64, 176-byte frame | **6.5** (range 6.4 – 6.5) |
| speedup | **70x** |

At BotW's measured **3,143 switches/frame**, that is **1.43 ms/frame today → 0.02 ms**, a saving of 1.41 ms — 2.8% of a 49.9 ms frame, 4.3% of the 33.3 ms frame the shrine actually runs at. The estimate was ~35% high on `swapcontext` and pessimistic on the replacement.

**It would not move the frame rate in the scene we can measure.** In gameplay the frame decomposes as **13.91 ms of work (p99 22.50 ms) inside a 49.90 ms frame** quantised to three vsync periods; the work already fits in *one* 16.68 ms period with 16.6% headroom and is not what holds the frame at three. Taking 1.4 ms out of a stage with 35 ms of slack crosses no boundary. This is the same shape as the `DeviceShared` buffer-cache change: real, measurable, and worth doing for power and headroom rather than for fps. **Do not schedule it as a frame-rate fix.**

*(The first version of this paragraph quoted 12.64 ms of work in a 33.27 ms frame with 24% headroom. Those were whole-file medians over a run that is ~46% title-and-save-menu frames — i.e. mostly menu figures. The conclusion is unchanged; the numbers were not the scene's.)*

Note the microbenchmark is the *right* instrument here and an in-process probe is not: `Fiber::Switch` does not return until the fiber is resumed, so a scope timer around it measures descheduled time. Two corrections to the text below, found while measuring: the `seq_cst` fences are at `FiberUnix.cpp:50` and `:52` (not `:52`/`:54`), and while it is true that `CemuUtil` cannot reach telemetry without a new link edge, the reason is not a dependency cycle — **nothing links `CemuUtil` except `CemuBin`**. The objection is that the edge would drag the fiber layer onto `CemuComponents` → `CemuGui` → `CemuWxGui` → wxWidgets.

### Register list

Save/restore: **x19-x28, x29 (FP), x30 (LR), sp**, and **d8-d15** (only the *low 64 bits* of v8-v15 are callee-saved; the upper halves are caller-saved).
Do **not** save: x0-x17, v0-v7, v16-v31, NZCV.
Do **not touch x18** — reserved by the Darwin kernel.

Note x27 (instance data) and x28 (memory base) are in the saved set. They're only live inside JIT code, and fiber switches always occur outside it, but saving them is free and removes a class of reasoning.

**Save FPCR** (2 instructions: `mrs`/`msr`). It is currently constant per host thread (set once in `enableFlushDenormalsToZero`, `coreinit_Thread.cpp:28-41`), so this is dead weight today — but if FPCR ever becomes fiber state, forgetting this is a brutal bug. Do **not** save FPSR (guest exception bits live in `hCPU->fpscr`). If `msr fpcr` shows up in a profile, gate it on "has any guest thread written FPSCR rounding bits".

### Frame (176 bytes, 16-aligned)

```
0x00 x19,x20   0x10 x21,x22   0x20 x23,x24   0x30 x25,x26
0x40 x27,x28   0x50 x29,x30   0x60 d8,d9     0x70 d10,d11
0x80 d12,d13   0x90 d14,d15   0xA0 sp,fpcr
```
11 `stp` + 11 `ldp` + 2 sp moves + `mrs`/`msr` ≈ 26 instructions, no syscall.

### Stacks

Replace `malloc(2MB)` (`FiberUnix.cpp:12-13`) with:
```
mmap(NULL, GUARD + 2MB + GUARD, PROT_NONE, MAP_PRIVATE|MAP_ANON, -1, 0)
mprotect(base + GUARD, 2MB, PROT_READ|PROT_WRITE)
```
with **`GUARD = 16384`** (macOS 26 / Apple Silicon page size — not 4096). Guard pages both below *and* above. `munmap` on destroy; `madvise(MADV_FREE)` on the body for pooled fibers.

Today a guest stack overflow silently corrupts the heap. **This is a correctness fix, not a perf one.**

### Correctness hazards

1. **The `seq_cst` fences** (`FiberUnix.cpp:52`, `:54`). A fiber switch is not a thread migration, and a hand-written asm switch is an opaque external call the compiler cannot reorder memory across. They are almost certainly unnecessary. **Keep them anyway for now** — a `dmb ish` (~10 cycles) is noise next to the `sigprocmask` syscall you're deleting, and removing them is a separate, *provable* change. Comment and revisit after measuring.
2. **`uc_link = &ctx[0]` is self-referential** (`:19`). If a fiber entry point ever returns, the current code `setcontext`s into itself. In the replacement, build the initial frame so `x30 = fiber_exit_trap` which does `__builtin_trap()`.
3. **Initial frame:** set `x19 = entryFn`, `x20 = userParam`, `x30 = fiber_trampoline`, `sp = (stackTop & ~15)`. `fiber_trampoline` does `mov x0, x20; blr x19; b fiber_exit_trap`. **This kills the `#ifdef __arm64__` split-the-64-bit-param hack** at `FiberUnix.cpp:18-23` and its counterpart `__OSFiberThreadEntry(uint32 _high, uint32 _low)` at `coreinit_Thread.cpp:1341-1348`.
4. **Stack alignment:** AAPCS64 requires sp 16-aligned at all times; Darwin enforces it for signal delivery.
5. **Unwinding / CFI — this is the real cost of hand-rolling.** Without it, `lldb bt` from inside a guest fiber walks into garbage, crash reports become useless, and — critically — **Instruments' sampler produces junk stacks, silently degrading the harness from 0.2**.
   - `cemu_fiber_swap` never moves its own sp, so plain `.cfi_startproc` / `.cfi_endproc` makes the switch transparent to the unwinder.
   - The discontinuity is at `fiber_trampoline`. Mark it `.cfi_startproc`, `.cfi_def_cfa sp, 0`, `.cfi_undefined x30` so the unwinder terminates cleanly at the fiber base instead of chasing a garbage LR. `.cfi_endproc`.
   - Write it as a `.S` file (the assembler emits `__eh_frame`), `.p2align 2`, `.no_dead_strip`, in `__TEXT,__text`.
   - **Add an explicit test:** break inside a guest HLE call, `bt` must show `fiber_trampoline` as the root and must not crash. Then re-validate the 0.2 JIT symbol map end-to-end.
6. **TSan** will not understand the switch; if you use it, add `__tsan_switch_to_fiber`. Probably out of scope.
7. `sCurrentFiber` is `thread_local` — unchanged, since fibers stay on one host thread, and the asm call is an optimisation barrier for the TLS address.

**Effort M. Verification:** ping-pong microbenchmark (10^6 switches; expect ~600 ns → ~15 ns); a register fuzz test (fill all callee-saved regs with known patterns, switch through M fibers N times, verify — **R5**); a deliberate guest-stack-overflow test that must SIGSEGV at the guard, not corrupt the heap.

## 4.3 16 KB page correctness

- **`MemMapperUnix.cpp:62-67` `FreeMemory` does no rounding** while `AllocateMemory:44-56` rounds the base down (but not the end up). On 16 KB pages a non-aligned `mprotect` addr returns `EINVAL` and the free **silently fails** — the return value is discarded. Fix both to round base down and end up, and **check the return value**.
- **`MMU.h:77` `cemu_assert_debug((endAddress & 0xFFF) == 0)`** in `MMURange::setEnd` bakes in 4 KB. Graphic packs in the wild specify 4 KB-aligned ends, so *don't* tighten the assert to 16 KB (compat regression). Instead: keep the declared-range assert, and round the actual `mprotect` call outward to `MemMapper::GetPageSize()`, with an overlap check against neighbouring ranges (they are ≥ 32 MB apart in practice).
- **`CORE0/1/2_LC`** (`MMU.cpp:120-122`): bases 0xFFC00000/0xFFC40000/0xFFC80000 are 16 KB-aligned ✓; size 0x5000 is not a multiple of 0x4000. This *works today by accident* because Darwin's `mprotect` rounds length up to a whole page. Make it explicit: **size 0x8000**. The next range is 0x40000 away, no overlap.
- **`HIGHMEM`** (`MMU.cpp:123`): base 0xFFFFF000 is **not** 16 KB-aligned (0xFFFFF000 % 0x4000 = 0x3000). `AllocateMemory` rounds down to 0xFFFFC000 and the kernel rounds the length up, so `[0xFFFFC000, 0x100000000)` ends up RW — correct coverage for Project Zero's 0xFFFFFFFE write, but accidental, and it silently makes 0xFFFFC000-0xFFFFEFFF writable when it should fault. **Fix: declare it as `{0xFFFFC000, 0x00004000}`.**
- **The durable fix:** a boot-time audit loop over `g_mmuRanges` that logs any base/size not a multiple of `MemMapper::GetPageSize()`, logs every rounding-induced expansion, and asserts no two *rounded* extents overlap (**R9**). This catches graphic-pack mappings too, which is where the next such bug will come from.

### Should the 512 MB jump table be shrunk on an 8 GB machine? **No.**

It is 512 MB of **reserved VA** (`PPC_REC_ALIGN_TO_4MB(0x10000000/4)` × 8 bytes), committed lazily in 8 MB chunks per 4 MB of PPC address space (`PPCRecompiler_reserveLookupTableBlock`, `PPCRecompiler.cpp:502-524`). Real RSS is 2× the size of guest code actually executed — 40-80 MB for a large title. That is acceptable, and:

- VA is free on arm64.
- The flat table is **one `ldr` from `[x27 + guestAddr*2]`** (`BackendAArch64.cpp:893-897`). Any hashed or two-level scheme adds ≥2 instructions and a dependent load to *every* guest function call, return, and indirect branch (`MACRO_BL`, `MACRO_B_FAR`, `MACRO_B_TO_REG`) — the hottest path in the emulator.
- 8 GB pressure is dominated by texture and shader caches, not this.

**Do not touch it.** Effort **S** for the rest of 4.3.

## 4.4 JIT lifetime — epoch-based reclamation

`PPCRecompiler_deleteFunction` (`PPCRecompiler.cpp:571-590`) unlinks jump-table entries and range-store entries but leaks `func->x86Code` and the `PPCRecFunction_t` itself (`// todo - free x86 code`, `:589`). `PPCRecompiler_cleanupAArch64Code` (`BackendAArch64.cpp:1622-1628`) exists and is never called. Monotonic leak across a session for any title that hot-patches code (RPL DynLoad, `OSCodeGen`, Cemuhook patches).

**The hazard:** `deleteFunction` runs on whatever guest thread called `PPCRecompiler_invalidateRange`, under `recompilerSpinlock`, while other guest cores may be *executing inside* that code or holding its address in x30 / on the host stack. Freeing immediately is a use-after-free.

**Design — epoch-based deferred reclamation:**
- `std::atomic<uint64> g_jitEpoch`; `std::atomic<uint64> g_coreEpoch[3]` initialised to `UINT64_MAX`.
- In `PPCRecompiler_enter` (C, **not** the emitted stub): `g_coreEpoch[core].store(g_jitEpoch.load(acquire), release)` before `PPCRecompiler_enterRecompilerCode`, `UINT64_MAX` after.
- `deleteFunction` pushes `{func, ++g_jitEpoch}` onto a retire list instead of freeing.
- A reclaim pass runs from `PPCRecompiler_thread` (`PPCRecompiler.cpp:445-490`) on its existing 10 ms wakeup, under the spinlock: free everything whose retire epoch `< min(g_coreEpoch[i])`.
- Free = `PPCRecompiler_cleanupAArch64Code(func->x86Code, func->x86Size)` + `delete func`. **Requires 1.3 landed first**, because the allocator must get the correct size for `protect()`.

**Allocator ownership note (coordinate with the other agent):** `AArch64Allocator` (`BackendAArch64.cpp:63-97`) is a stack local whose `MmapAllocator s_allocator` is `static inline`, so state survives — the `setFreeDisabled(true)` / fresh-allocator-for-free dance is fragile but functional. Since W^X/MAP_JIT allocator ownership is theirs, state the interface you need and let them implement it:
```
void* JitAlloc(size_t);
void  JitFinalize(void* code, size_t size);   // W^X flip + sys_icache_invalidate
void  JitFree(void* code, size_t size);
```
Then delete `AArch64Allocator` and the `setFreeDisabled` mechanism entirely.

**Effort M. Payoff: memory, not speed** — but on 8 GB with a long session it's the difference between stable and not. It also enables shrinking the JIT arena, which reduces the `±128 MB` jump-range rejections at `BackendAArch64.cpp:206-248`.

**Verification (R7):** in debug builds, poison freed JIT pages with `brk #0xdead` for a grace period before returning them; assert no core epoch is inside a retired range at free time. Stress: invalidate-and-recompile a range in a loop for an hour, watch the JIT region via `vmmap`.

---

# Phase 5 — Opportunistic

## 5.1 AES on FEAT_AES — **best payoff/effort ratio in the whole plan**

`src/util/crypto/aes128.cpp`: full AES-NI path at `:603-800` under `#if defined(ARCH_X86_64)`, dispatch at `:839-856`. On arm64 it falls back to table-driven software AES (`__soft__AES128_CBC_decrypt`). This gates NUS/WUD title decryption and ZArchive throughput — i.e. install and load times, which is the most user-visible number in the emulator.

Port to `<arm_neon.h>`:
- Encrypt: `state = vaesmcq_u8(vaeseq_u8(state, rk[i]))` for rounds 0..8, then `veorq_u8(vaeseq_u8(state, rk[9]), rk[10])`.
- Decrypt: `state = vaesimcq_u8(vaesdq_u8(state, rk[i]))`, final `veorq_u8(vaesdq_u8(state, rk[1]), rk[0])`, using the equivalent-inverse-cipher schedule (`vaesimcq_u8` applied to middle round keys) — mirror what the x86 path does with `_mm_aesimc_si128`.
- **The gotcha:** ARM `AESE` performs *AddRoundKey first*, then SubBytes+ShiftRows. Intel `AESENC` does ShiftRows+SubBytes+MixColumns+AddRoundKey. **The round-key indexing is offset by one relative to the x86 code.** Getting this wrong yields plausible-but-wrong ciphertext. Write it against FIPS-197 test vectors, not against the x86 source.
- **CBC decrypt is fully parallel** across blocks (the XOR chain is on the output side). Process **4 blocks interleaved** — that's where the throughput is on Apple cores.
- Dispatch: FEAT_AES is architectural on all Apple Silicon. Unconditional, no `g_CPUFeatures` query. Delete the whole `#if defined(ARCH_X86_64)` branch at `:838-856`.
- Build: `-march=armv8-a+crypto` on that TU or `__attribute__((target("crypto")))`.

**Effort S-M. Payoff: ~4-5× on encrypted-format install/load** (table AES ~1-2 cycles/byte → 4-way `vaese` ~0.4). Independent of every other item; can be done by anyone at any time. **Verification:** FIPS-197 + NIST CBC vectors as a unit test, plus a known-WUD decrypt checksum.

## 5.2 `psq_l` / `psq_st` F32 fast path via `fcvtl`/`fcvtn`

`PPCRecompiler_isUGQRValueKnown` (`IMLOptimizer.cpp:~245-258`) only optimises GQR0 = 0 → `TYPE_F32`. That is the overwhelmingly common case, and it is the hottest FP memory op in Wii U graphics code. Today `PPCRecompilerImlGen_EmitPSQLoadCase` (`PPCRecompilerImlGenFPU.cpp:764-771`) emits two separate `FPR_LD_MODE_SINGLE` loads = ~10 host instructions after 2.2.

Replacement, once a pair vreg exists (5.3) or even just using a temp Q register:
```
ldr   d,  [base, idx]      ; two big-endian f32s
rev32 v.8b, v.8b           ; byte-swap each lane
fcvtl v.2d, v.2s           ; expand both to double
```
**4 instructions total including address setup, vs ~10.** Store is the mirror: `fcvtn v.2s, v.2d; rev32 v.8b, v.8b; str d`.

**This is a Phase-B-sized change with a Phase-D-sized payoff.** Do it. Effort **M**.

## 5.3 Paired-single NEON — do the narrow version, **not** the `IMLReg::Offset` revival

### Verdict on the big version: don't do it

Reviving `IMLReg::Offset` (the field at bits 19-23 whose constructor ignores it, `IMLInstruction.h:33-38`, and whose move-ctor is `DEBUG_BREAK`, `:41-47`) means teaching the linear-scan allocator about **sub-register views and partial writes**. `IMLUsedRegisters` has no concept of a partial def; DCE (`IMLOptimizer_RemoveDeadCodeFromSegment`, `:571-620`) would treat a lane-0 write as a full kill and delete live lane-1 data. The allocator already carries seven TODOs about suboptimal range splitting (`IMLRegisterAllocator.cpp:581`, `:614`, `:807`, `:1218`, `:1973`, `:1997`, `:2081`) and an "approximation" comment on its cost model (`IMLRegisterAllocatorRanges.cpp:575`). Adding partial-def semantics on top of that is exactly the change that produces silent, rare, game-specific miscompiles you will spend months chasing with no upstream to diff against.

**And the ceiling is low.** After 3.2 (FMA), `ps_madd` is already 2 instructions. Going to vector form does not help, because **AArch64 NEON has no non-destructive 3-operand vector FMA** — `FMLA Vd.2D, Vn.2D, Vm.2D` is destructive accumulate (`Vd += Vn·Vm`). PPC `ps_madd` maps to it only when `frD == frB`; otherwise you need `mov v, vB` first, giving 2 instructions — **identical to two scalar `fmadd`**. So the entire `ps_madd`/`ps_msub`/`ps_nmadd`/`ps_nmsub` family, which is most of the paired-single traffic, gets **zero** from vectorization.

### What to do instead: narrow pair vregs, no new register format

**Key enabling fact I verified: the codebase already has F64 vregs that are secretly 128-bit.** `r_name`/`name_r` for `PPCREC_NAME_TEMPORARY_FPR0` use `fpReg<QReg>` with `ldr q`/`str q` (`BackendAArch64.cpp:448`, `:529`) while every other F64 name uses `DReg`. So "this F64 vreg holds a 128-bit pair" is an existing, working convention — encoded purely in the *name* and the *instruction type*, not in `IMLRegFormat`.

Therefore: **add no new `IMLRegFormat`, touch no register-allocator code, keep `IMLPhysRegisterSet` at 64 bits.**

1. New name space `PPCREC_NAME_FPR_PAIR = 4600 + N` (N = guest FPR index). Backend `r_name`/`name_r` handle it with `ldr q` / `str q` at `offsetof(fpr) + 16*N` — a **single instruction for both halves**, vs two `ldr d` today. Add `alignas(16)` to `FPR_t fpr[32]` in `PPCState.h:47` (it currently lands at offset 136, 8-aligned) — unaligned `ldr q` works on arm64 but aligned is free.
2. New instruction types `PPCREC_IML_TYPE_FPR2_R_R_R` (and `_R_R`, `_R`) with ops ADD/SUB/MUL/DIV/NEG/ABS/ROUND_TO_SINGLE. Backend emits `fadd v.2d`, `fsub v.2d`, `fmul v.2d`, `fdiv v.2d`, `fneg v.2d`, `fabs v.2d`, and `fcvtn v.2s / fcvtl v.2d` for the paired round-to-single (**which replaces two 2-instruction `fcvt` pairs with two instructions total**).
3. Two explicit conversion instructions: `FPR_PAIR_EXTRACT_LANE(dstF64, srcPair, lane)` → `mov(dReg, vSrc.d[lane])`, and `FPR_PAIR_INSERT_LANE(dstPair, srcF64, lane)` → `mov(vDst.d[lane], dSrc)`. From the RA's perspective these are ordinary reads and read-modify-writes of whole registers — **the existing `IMLUsedRegisters` model expresses them exactly** (`INSERT_LANE` sets both `readGPR` and `writtenGPR` on the destination, same as `PPCREC_IML_OP_FPR_ADD` already does at `IMLInstruction.cpp:270-277`).
4. **Coherency, the only real cost.** A guest FPR now has two possible IML representations. Enforce: *the pair vreg and the two half vregs for the same guest FPR are never simultaneously live.* Track a per-guest-FPR "current representation" during IR generation of a basic block. When a scalar op needs a half of an FPR currently in pair form → emit `EXTRACT_LANE` and switch; when a paired op needs an FPR in half form → emit two `INSERT_LANE`. **At every segment boundary, force back to halves (canonical).** This is bookkeeping in IR gen only; the RA, DCE, and optimizer see ordinary whole-register operations. Paired ops cluster in real code (a vertex transform is 10 consecutive `ps_*`), so switches amortise to ~zero.
5. **`IMLOptimizer_OptimizeDirectFloatCopies`** (`IMLOptimizer.cpp:19-142`) operates on whole regs and scans for `FPR_LOAD/STORE MODE_SINGLE` — pair vregs never appear in those instruction types, so the pass is unaffected. Verify that `PPCRecompiler_optimizeDirectFloatCopiesScanForward`'s `CheckRegisterUsage`-based break (`:74-84`) correctly sees pair vregs as distinct reg IDs. It will.
6. **Scope: `ps_add`, `ps_sub`, `ps_mul`, `ps_div`, `ps_neg`, `ps_abs`, `ps_mr` only** (`PPCRecompilerImlGenFPU.cpp:1089`, `:1128`, `:1153`, `:1193`, `:1486`, `:1508`, `:1567`). Do **not** extend to the `ps_madd` family (see the FMLA argument above). Do **not** extend to `ps_sum0/1`, `ps_merge*`, `ps_sel`, `ps_cmp*` — those are lane-shuffle ops where the half representation is already natural.

**Effort M-L. Gate behind a compile-time flag so it can be A/B'd on the 0.1 metric.** Only do this after 3.2 and 5.2 have landed and the audit shows remaining headroom in `ps_*`-heavy titles. **Risk R6.** If the measured win is under ~3% of host instructions in a 3D title, stop — the coherency bookkeeping is not worth the ongoing maintenance burden on a fork.

## 5.4 AArch64 peephole pass

There is currently zero backend-specific peephole optimization once `IMLOptimizerX86_SubstituteCJumpForEflagsJump` is deleted. Add `IMLOptimizerAArch64_PeepholePass(IMLOptimizerRegIOAnalysis&, IMLSegment&)`, called from `IMLOptimizer_StandardOptimizationPassForSegment` (`IMLOptimizer.cpp:703-710`) — exactly where the x86 pass was, i.e. after DCE and before `PPCRecompiler_NativeRegisterAllocatorPass`. Reuse the `IMLUtil_*` helpers you kept in 1.1 and `regIoAnalysis.IsRegisterNeededAtEndOfSegment`.

Contents, in priority order:
1. **`FPR_ASSIGN` coalescing** — `FPR_ASSIGN d, a` where `a` is dead after and `d` is written only here → rewrite subsequent uses of `d` to `a`, drop the move. The FPU IR gen emits these liberally. **Check first whether the RA already coalesces**; if it does, skip. Note that 3.2 (FMA) removes a large fraction of them anyway, so **measure after 3.2 lands**.
2. **Integer `madd`/`msub`** — `MULTIPLY_SIGNED t,a,b` + `ADD/SUB r,c,t` with `t` dead after → a fused op. Requires a new integer `PPCREC_IML_TYPE_R_R_R_R` (only the FPR variant exists today). **Payoff: low** — PPC emits `mullw`+`add` for `a*b+c` and the frequency in game code is modest. Rank it last. Effort M.
3. **Redundant `ROUND_TO_SINGLE_PRECISION_BOTTOM` elimination.** Each one is `fcvt s,d; fcvt d,s` = 2 instructions, and `PPRecompilerImmGen_roundToSinglePrecision` is emitted after *every* `*s` and `ps_*` op. Only two eliminations are semantically safe: back-to-back duplicates with no intervening write, and a round immediately following a load that already produced a single (the `flags2.notExpanded` path). Modest but cheap.

The `ldp`/`stp` fusion from 2.5 deliberately lives in the backend emit loop, not here — keeping it out of IML avoids touching DCE, the RA, and the debug printer.

---

# Explicitly NOT doing / traps

| Item | Why |
|---|---|
| **Reviving `IMLReg::Offset` / sub-register views in the RA** | *The* trap. Partial-def semantics on a linear-scan allocator that already has 7 open TODOs and an "approximation" cost model. Silent miscompiles, no upstream to diff against. Ceiling is low (see FMLA argument). |
| **Vector FMA (`fmla v.2d`) for `ps_madd`** | NEON has no non-destructive 3-operand vector FMA. `mov + fmla` = 2 instructions = identical to two scalar `fmadd`. Zero win. |
| **`ccmp` chaining** | The CR model is one byte per bit with `cset` materialising booleans. Fusing AND-of-two-comparisons across CR-bit vregs is medium effort for low payoff. |
| **`cinc`/`csinc` for carry** | `cmp regCarry,1; adcs; cset` (`BackendAArch64.cpp:670-675`) is already near-optimal for materialising carry in/out from a byte. `cinc` doesn't apply. |
| **FPCR rounding-mode plumbing (guest `mtfsf`)** | Behaviour change with **no** perf benefit and a large compat surface. Guest rounding modes are ignored today; keep it that way. (R4) |
| **Shrinking the 512 MB jump table** | VA is free; the flat table is 1 `ldr` on the hottest path; RSS is 2× executed guest code, ~40-80 MB. |
| **Reducing cycle-accounting frequency** | Changes scheduling granularity; can starve threads spinning in straight-line code. |
| **Widening `IMLPhysRegisterSet` past 64 bits** | Not needed — 5.3 adds no new `IMLRegFormat`, so the pool stays at 56/64 used. Don't do it speculatively. |
| **`PPCRecompiler_optimizePSQLoadAndStore`** (`IML.h:9`) | Declared, never defined, never called. Delete the declaration. |
| **`_mm_mfence` in timer code** | Deleted wholesale by 4.1; do not port it. |

---

# Risk register

| # | Change | Silent failure mode | Detection |
|---|---|---|---|
| **R1** | FMA (3.2) | FP results differ in the last ULP on every `fmadd`-family op. Games hashing float bits, seeding RNG from floats, or accumulating physics across frames diverge → replay/ghost desync, animation drift. Historically the highest-risk class in emulator FP work; note the existing `PS_NMADD` comment about Splatoon needing denormal flushing (`PPCRecompilerImlGenFPU.cpp:1349-1351`). | 0.3 differential harness over randomized operands incl. denormals/±0/±inf/sNaN/qNaN; **explicit unit tests for the `fmsub`↔`fnmsub` crossover**; physics-heavy per-title smoke test; ship behind a `GameProfile` toggle; bisect with `--ppcrec-range`. |
| **R2** | Cycle counting in a register (3.1) | A missed write-back changes `remainingCycles` at an HLE boundary → thread scheduling order changes → nondeterministic hangs reproducing once in 100 boots. **Worst failure mode in this plan** — it manifests through the scheduler, not the CPU. | Debug-build shadow `remainingCycles` written via an extra `NAME_R` after every `SUB`; assert equality at every `leaveRecompilerCode` and on entry to `PPCRecompiler_virtualHLE`. Per-thread cycle-accounting counter dumped per second, compared before/after. |
| **R3** | PPCTimer rewrite (4.1) | Guest clock *rate* shift → in-game timers, animation speed, audio sync, `OSSleepTicks` durations all off. A 1% error is invisible in a smoke test and ruinous over an hour. | Assert the exact 3315/64 rational against `cntfrq_el0` at init; CI check of `OSGetSystemTime()` vs `CLOCK_MONOTONIC` drift over 60 s, fail >100 ppm; stopwatch an in-game clock. |
| **R4** | FPCR rounding plumbing | Would change FP results everywhere for zero perf gain. | **Don't do it.** |
| **R5** | Fiber switch (4.2) | Missing a callee-saved register (especially d8-d15) → rare corruption indistinguishable from a game bug. Missing CFI → useless backtraces *and* junk Instruments profiles, silently invalidating the 0.2 harness. | Register-pattern fuzz across M fibers × N switches; ASan with stack-use-after-return; deliberate guest stack overflow must trap at the guard page; `lldb bt` test; **re-validate the JIT symbol map after landing**. |
| **R6** | Pair vregs (5.3) | Stale ps0/ps1 from bad representation-switch bookkeeping → wrong geometry, invisible until a specific game path. | Debug mode: at every basic-block end, materialise both representations to memory and compare. Static IML dump diff. |
| **R7** | JIT free (4.4) | Use-after-free of executing code. Loud crash is the *good* outcome; the bad one is arena reuse while a stale return address points into it → executing the wrong function. | Poison freed pages with `brk #0xdead` for a grace period; assert no core epoch inside a retired range; hour-long invalidate/recompile stress. |
| **R8** | `getSize()` fix (1.3) | Under-flushing the I-cache → stale-instruction crashes only on cold paths, only on some cores. Currently masked by `getMaxSize()` being larger. | Verify `readyRE()`'s flush range; add an unconditional `sys_icache_invalidate(code, size)` after finalize. |
| **R9** | 16 KB rounding (4.3) | Rounding a range outward makes a neighbouring guest page writable that should fault — hides real guest bugs, lets buggy games corrupt adjacent state. | Boot-time audit logging every rounding-induced expansion; assert no two rounded extents overlap. |
| **R10** | Bitmask immediates (2.4) | A wrong `isLogicalImm32` predicate encodes a *different* constant → wrong `andi.` results. Silent and pervasive. | Enumerate all **5334** encodable 32-bit values + 10^7 random rejects; assert `predicate(v) ⟺ xbyak encodes and decodes back to v`. |
| **R11** | Deleting the LL/SC atomic fallback (1.1) | None on Apple Silicon (FEAT_LSE architectural). Would UDF on a non-LSE core. | Static assert on target arch; arm64-Apple-only by decree. |

---

# Execution order (payoff / risk)

```
0.  Harness            0.1 static jit-audit CSV + opcode histograms   [M]  ← blocks all claims
                       0.2 xctrace + JIT symbol map                    [M]
                       0.3 interpreter↔JIT differential test           [M]  ← gates 3.2, 5.3
1.  Subtract           1.3 getSize()/cursor fix                        [S]  ← do before 0.1 lands
                       1.1 x86 purge                                   [L]
                       1.2 betype __builtin_bswap + bit_cast           [S]
2.  Codegen            2.1 offset-0 addressing fold                    [S]  ← biggest instr-count win
                       2.2 FP load/store via rev32/rev64               [S-M]
                       2.3 rev16                                       [S]
                       2.4 bitmask immediates (verify xbyak first)     [S-M]
                       2.5 ldp/stp name fusion (backend emit loop)     [M]
3.  Structural         3.1 cycle counting as IML register              [M]  ← biggest loop win, R2
                       3.2 FMA (correctness + perf)                    [M]  ← R1
4.  Infrastructure     4.1 PPCTimer rewrite                            [S-M] ← kills 3s boot stall
                       4.2 fiber switch + guard pages                  [M]  ← R5, revalidate 0.2
                       4.3 16 KB page audit + MemMapper rounding       [S]
                       4.4 JIT epoch reclamation                       [M]  ← needs 1.3 + allocator agent
5.  Opportunistic      5.1 AES FEAT_AES                                [S-M] ← best payoff/effort, independent
                       5.2 psq_l/psq_st F32 via fcvtl/fcvtn            [M]
                       5.3 pair vregs, ps_add/sub/mul/div only         [M-L] ← only if 5.2 shows headroom
                       5.4 peephole pass (coalescing, madd, rounds)    [M]  ← measure after 3.2
```

5.1 has no dependencies on anything else and the best payoff/effort ratio in the plan for user-visible time — hand it off in parallel on day one.

---

### Critical Files for Implementation

- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Espresso/Recompiler/BackendAArch64/BackendAArch64.cpp`
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Espresso/Recompiler/PPCRecompilerImlGenFPU.cpp`
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Espresso/Recompiler/IML/IMLInstruction.h`
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Espresso/Recompiler/PPCRecompiler.cpp`
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/Cafe/HW/Espresso/PPCTimer.cpp`
- `/Users/patricedery/Coding_Projects/TesseraEmu/src/util/Fiber/FiberUnix.cpp`