# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this fork is

A hard fork of Cemu (Wii U emulator, C++20) retargeted at **Apple Silicon + macOS 26 only**. Upstream Cemu is portable; this fork is deliberately not. Three backends became one, two architectures became one:

- **arm64 only.** `precompiled.h` `#error`s on any other target. `BackendX64/` and all x86 IML machinery are deleted.
- **macOS 26.0 minimum.** Verify with `otool -l bin/... | grep -A4 LC_BUILD_VERSION` → `minos 26.0`.
- **Metal only.** OpenGL, Vulkan/MoltenVK, the GLSL shader emitter and glslang are deleted. The binary links Metal and QuartzCore, nothing else graphical.

Do not reintroduce portability shims, `#ifdef ARCH_X86_64`, or a second renderer. If something looks like it needs a runtime arch/backend check, it doesn't.

Planning docs live in `docs/porting/`. `00-master-plan.md` is the staged plan and risk register; the three numbered files are the detailed per-workstream designs (foundation/platform, CPU/JIT/memory, graphics/Metal). **Read the relevant one before touching that subsystem** — they contain verified line-level findings that are expensive to rediscover.

**Testing docs live in `docs/testing/`.** `00-test-strategy.md` is the plan of record for the whole
test effort — what exists in the world (mostly nothing, for Wii U), what we built, the provenance and
licence of every borrowed artefact, and an honest status table. The test suites themselves are Wii U
homebrew and need a toolchain; `testing/toolchain/` builds one, including for the case where
devkitPro's installer and package host are both unavailable.

**`docs/status/index.html` is the fork's live record, and keeping it current is a standing obligation — not optional cleanup.** Every item attempted since the fork point and what it measured, in one filterable page. **When a work item lands, add an entry to `docs/status/ledger.json` naming its commits, run `python3 docs/status/build-status.py`, and commit the regenerated HTML alongside it.** A negative result is a first-class entry: `refuted` (tested against a control, false), `cancelled` (gated out before being built) and `reverted` (built, measured, removed) are distinct and all belong on the page.

**Read `.claude/rules/status-tracker.md` before editing the ledger** — it is the full rule. The two things worth knowing up front: the generator derives the commit list, diffstat, baseline table and counter totals *from the repo*, so never type those into the ledger; and a verdict is one line plus a `ref`, because `docs/porting/00-master-plan.md` owns the reasoning and a second copy of an argument is a second thing to keep in sync.

CI runs `build-status.py --verify` on every push and reports, in the job summary, any commit that landed without a ledger entry. It advises rather than blocks, because some lag is structural: a commit cannot name its own hash in the ledger it contains, so the tip is always unclaimed and is reported separately. `--verify` *does* exit nonzero on a malformed ledger or an unresolvable hash. `--check` is a local pre-commit convenience only — it cannot gate CI, because the page stamps itself with the commit that carries it and is therefore always one commit behind by construction.

## Build and run

```sh
export VCPKG_DEFAULT_BINARY_CACHE="$HOME/.cache/vcpkg"        # first build is ~25 min without this
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DMACOS_BUNDLE=OFF
cmake --build build                                            # ~2 min clean on an M2
./bin/TesseraEmu_relwithdebinfo --verbose -g "Roms/<title>.wux"
```

`MACOS_BUNDLE=OFF` for day-to-day work — bundle builds are for signing/entitlements. `RelWithDebInfo` deliberately has **LTO off**: it keeps relinks fast and keeps `.o` files as real Mach-O, so `llvm-objdump` can inspect generated code. `Release` turns ThinLTO on and the objects become LLVM bitcode.

Useful launch flags (`src/config/LaunchSettings.cpp` has the full list): `--force-interpreter` to bypass the recompiler when isolating a JIT bug, `--ppcrec-lower-addr` / `--ppcrec-upper-addr` to bisect which recompiled function broke, and
`--telemetry <file.jsonl>` / `--telemetry-label <name>` to record the per-frame counters that
everything below is measured with, `--telemetry-areas <cpu,gpu,mem,accuracy>` to narrow them, and
`--forward-console-logging` to route the guest's `OSReport` to stdout — which is how both test
suites report their results.

There is **no automated test suite for graphics**; verification there is still: it builds, it boots a
title, the frame looks right, and nothing new appears in the log.

**The CPU now has one.** `testing/cpu-tests/` runs `ppc750cl.s` — 23,502 lines of public-domain
PowerPC assembly validated against real Espresso silicon — as Wii U homebrew, needing no game
image, console, keys or SDK. First result: **1,030 failures, recompiler and interpreter
identical (zero unique to either arm)**, of which 676 are FPSCR state bits this emulator does not
maintain and **354 are wrong values — only 3 of them integer**. So the AArch64 backend is exactly
as correct as the interpreter, and the real defects are in shared FP/paired-single semantics.
There is also `testing/graphics-tests/`, a render-pass self-dependency reproducer that builds and
runs but whose counters currently read **zero** — an unresolved result, not a passing test. See
`docs/testing/00-test-strategy.md` and `testing/graphics-tests/README.md`.

## Verifying a change

A green build proves very little here. The real gate is booting a real title:

```sh
./testing/capture-scene.sh <pid> <scene-name>    # window-only frame + FPS/RSS/threads -> testing/golden/baseline.tsv
```

`testing/golden/baseline.tsv` is the committed record of every measurement; the PNGs and traces are gitignored (large, machine-specific). Requires `~/Library/Application Support/TesseraEmu/keys.txt` with the Wii U common key and the title's disc key — without it decryption fails and nothing boots. Game images live in `Roms/` (gitignored).

TesseraEmu writes no `log.txt` until clean exit, and `CemuApp::OnExit` calls `_Exit()`, so **`kill -9` loses buffered log and shader-cache writes.** Use `--verbose` and read stdout instead.

When a launch appears to hang at low CPU, it is almost always a modal dialog. Read it without screenshotting the user's screen:

```sh
osascript -e 'tell application "System Events" to tell (first process whose unix id is <pid>) to tell (first window whose description is "alert") to get value of every static text'
```

## Profiling — use xcprof, not raw xctrace

`xcprof` (Axiom, on PATH) wraps xctrace and emits structured output. It is the fastest way to get attributable numbers, and `compare` is what makes optimization claims defensible:

```sh
xcprof record --attach <pid> --preset cpu --time-limit 15s --no-prompt --output testing/traces/<name>.trace
xcprof analyze testing/traces/<name>.trace
xcprof compare testing/traces/before.trace testing/traces/after.trace
```

`--output` must stay inside the repo (its sandbox) or pass `--allow-external-output`. `xcsym crash <file>` symbolicates `.ips`/MetricKit crash reports.

### How to measure here without fooling yourself

- **A BotW telemetry run is two workloads, not one, and the split is ~50/50.** `drive-botw.sh`
  spends its first **~4,050 frames in the title and save menus** — 113 draws, 4.2 ms GPU, 33.27 ms
  frames — before ~4,750 frames of gameplay at 3,516 draws, ~16.5 ms GPU, 49.90 ms frames. `--skip=60`
  removes none of it. A median over the whole file lands wherever the phase ratio falls and describes
  no frame that occurred, and because two runs of the same script never have quite the same ratio,
  **that difference shows up as a counter delta that looks like a treatment effect**. It produced a
  confident "−6.2% GPU busy" for a change whose real effect is +0.5%. `testing/telemetry-report.py`
  now segments by draw load, prints the phases and analyses the longest; pass `--all` for the old
  behaviour, `--phase=N` to pick. Any BotW number in this repo predating that is suspect.

- **GPU-time counters soak, so their median depends on how long you recorded.** Slice the Korok
  gameplay phase into sixths and the *work* is flat — draws 3,472→3,518, render passes 202,
  attachment bytes 800 MB, guest core busy 9.4–9.5 ms — while **`gpu.busy_ns` climbs 16.9 → 17.8 ms
  and `gpu.readback_forcefinish_ns` climbs 6.5 → 7.5 ms**. Same frame, ~5% and ~15% more GPU time
  after four minutes of continuous play, reproduced in all three runs. Frame time, fps and
  `gpu.frame_critical_path_ns` drift **0.0%** because they are vsync-pinned, which is exactly why
  this hides: the headline numbers look rock-steady while the GPU counters underneath them walk. So
  **A/B arms must record for the same duration at the same point in the run**, or the longer arm
  loses on GPU counters for free. This is also why the drain counter was described as swinging "±2%
  between identical runs" — that was not noise, it was window placement, and today's three
  equal-length runs agree on it to ±0.4%.

- **Neither provenance stamp proves what the binary was built from.** `CMakeLists.txt:15` captures
  `git log --format=%h -1` at **cmake configure time** and bakes it into `-DEMULATOR_HASH`, so a
  binary compiled from `cebe17f` announced `build 064d9a7` — eight commits stale — in its window
  title and in the `build` field of every `--telemetry` header. `testing/capture-scene.sh` has the
  opposite failure: it stamps `baseline.tsv` with the *repo's* `git rev-parse HEAD`, which says
  nothing about the binary at all. Re-run `cmake -S . -B build …` before any measurement you intend
  to quote. The hash reaches every translation unit through `precompiled.h`, so refreshing it is a
  full rebuild.

- **A counter reading zero is not evidence until you check it has an increment site.** Sweeping all
  113 `TLM_COUNTER` declarations against `src/` found **14 that nothing ever writes**:
  `gpu.pipelines_compiled`, `acc.render_self_dependency`, and **all twelve `mem.*`**. The `mem.*`
  block covers buffer-cache uploads, texture-cache reloads and shader-cache hit/miss — exactly the
  subsystem behind the largest graphics finding on file, which had to be established by backtrace
  attribution *because these counters do not work*. A zero from an unwired counter is
  indistinguishable from a zero that means something. Check the hook exists, and prefer a positive
  control in the same code path (`gpu.depth_sampled_draws` reads 392/frame beside the
  self-dependency detector for exactly this reason).

- **The instrument only sees the scene you point it at.** A confirmed out-of-bounds write —
  geometry-shader texture bindings landing past the end of `MetalState::m_textures` — survived in a
  heavily measured fork because `gpu.draws_mesh` is **0** in the only scene anyone measures. Absence
  of a signal in BotW Korok is not absence of the bug. Where a defect is structural, prove it with a
  `static_assert` rather than waiting for a run to show it.

Two traps already caught in this repo — both produce confident, wrong numbers:

- **MK8's attract mode is not a fixed workload.** It cycles demo scenes with very different draw loads, so two traces taken minutes apart are not comparable, and neither are their sample counts. A first A/B here read as "4.4x faster" from sample counts; the real figure was 1.77x. To compare two implementations, **put a runtime toggle behind an env var, flip it every ~20 s inside one process**, discard any window that straddles a switch, and report the median of n≥5 per variant. Ranges that overlap mean you have not measured anything.
- **`xcprof compare` reports share of CPU, not absolute CPU.** If total CPU drops, everything that survives looks like a "regression" — it reported 15 of them for a change that cut CPU nearly in half. Use it to see *which frames left the profile*; use absolute process `cputime` over a fixed wall-clock window for the magnitude.

Prefer `ps -p <pid> -o cputime=` deltas over `%cpu` (a decaying average) for headline numbers.

- **`cemuLog_log` writes to `~/Library/Application Support/TesseraEmu/log.txt`, not stdout.** Grepping the process's redirected stdout for errors, or for your own instrumentation, silently finds nothing and reads as "clean".
- **`testing/capture-scene.sh` uses `screencapture -R`, which grabs a screen *region*** — TesseraEmu must be frontmost or you capture whatever is on top of it. Raise it first (`set frontmost of ... to true`).
- **A before/after pixel diff of the MK8 title screen proves nothing**: the "Press A to start" prompt pulses and the background animates, so ~22% of pixels differ between any two captures. Use targeted instrumentation to show a rendering change is live.

### Current baseline (2026-07-26, after the Stage 5 idle-wait fixes)

MK8, locked 60 FPS, **~104% of one core** (was ~184% before `a7ed8ed`+`612d064`). The profile is now dominated by real draw work: `LatteCP_itIndirectBufferDepr` 30.1% incl, `DrawPassContext::executeDraw` 19.2%, `MetalRenderer::draw_execute` 16.3%, and **`renderCommandEncoderWithDescriptor` 6.4% + `AGXG14GFamilyRenderContext init` 5.7%** — encoder/render-pass churn is the largest remaining target.

Historical note: an earlier baseline recorded `mach_continuous_time` at 47% self and attributed it to the graphics idle path. That attribution was wrong. The caller was `__OSThreadCoreIdle` → `__OSCheckSystemEvents`, an unbounded busy-wait in the *scheduler*. Both idle-wait bugs found in Stage 5 were invisible to the `hostInstrCount / ppcInstrCount` metric the plan ranked first — profile before picking a codegen target.

**MK8's attract mode drives itself into full demo races — no controller input needed.** This matters more than it sounds: the attract cycle spends most of its wall-clock on the *title card*, so a short trace or an unfiltered average silently measures a near-idle scene. Gate on `draws/f > 200` to isolate race frames.

Measured from `GPUEndTime - GPUStartTime` on every command buffer:

| scene | GPU ms/frame | % of 16.67 ms | passes/f | draws/f | draws per pass |
|---|---|---|---|---|---|
| title card | 2.6 – 3.0 | 16 – 18% | 29 | 51 | 1.75 |
| demo race, peak | **14.6** | **87.7%** | 222 | 1466 | 6.6 |

So the GPU is **not** idle in gameplay, and `draws-per-pass` is a healthy 7–9 rather than the alarming 1.75 the title card shows. An earlier revision of this file claimed the opposite from title-card-only data; `docs/porting/00-master-plan.md` carries the full correction. Before drawing any conclusion about graphics work, check which scene you sampled.

**Use BotW for graphics measurement, not MK8.** Breath of the Wild (US v208) at the Shrine of Resurrection with Link standing still, reachable unattended via `testing/drive-botw.sh`. It is *exactly* repeatable — `passes/f` holds within ±2 and `draws/f` within ±3 across every 60-frame window — which no MK8 scene is.

> **The two sets of Shrine numbers in this file disagree and have not been reconciled.** An older
> xcprof/capture-scene reading gives 28.6 FPS, 149 passes/f, ~1190 draws/f, GPU 15.6 ms; the
> telemetry table below gives 30.06 fps, 132 passes/f, 3784 draws/f, GPU 14.0 ms. Different
> instruments and different counting (draw *calls* vs draw *sequences*), and the GPU figures predate
> the `839466d` `busy_ns` fix. **Re-measure the Shrine with the current harness before quoting
> either.** The Korok Forest gameplay numbers below are current (n=3, 2026-07-31).

**Two BotW scenes, and the open world is the interesting one.** `testing/drive-botw.sh` reaches the
Shrine of Resurrection. A Korok Forest save (dense foliage + fog, the "roaming the open world" case)
is the heavier scene. Measured with the telemetry harness:

| | Shrine | Korok Forest |
|---|---|---|
| fps median | 30.06 | **20.04** |
| frame ms median / p99 | 33.27 / 49.92 | 49.90 / **49.97** |
| draws/frame | 3784 | **3517 – 3528** (fewer!) |
| render passes/frame | 132 | **202 – 203** |
| GPU busy/frame | 14.0 ms † | **17.1 ms** ‡ |
| GPU duty cycle | 42% † | **34%** |

† Shrine column measured before `839466d` and therefore inflated — see the note above.

‡ **Re-measured at `cebe17f`, n=3, and the Korok column is confirmed** — every figure reproduced
within 1.5%, frame time and fps to the decimal. The GPU number is the one that moved: 16.4–16.5 ms
became 17.12–17.17. That is **not** a regression, it is the soak described below — GPU time in this
scene rises ~5% over four minutes of continuous play at constant draw count, so the median depends
on how long you recorded.

**The forest is not GPU-bound and is not draw-bound** — it issues *fewer* draws than the shrine and
leaves the GPU idle 62% of the time. What it is, is **vsync-quantised**: 20.04 fps is exactly
59.94/3, 49.90 ms is exactly 1.5x the shrine's 33.27 ms, and p99 equals the median, so there is
essentially no variance (the frame is exactly three ~16.63 ms slots; an earlier revision wrote
that as "exactly 59.94/3", which is 19.98 fps, not 20.04 — the implied refresh is ~60.1 Hz).
Something misses the 33.3 ms deadline and the software vsync timer
(`LatteTiming`, host-driven vsync is a stub on this fork) drops it to the next whole division rather
than degrading smoothly.

> **Solved — and the Korok column above is conditional on a setting.** The thing missing the deadline
> is `GX2DrawDone`'s readback drain, not the renderer. With `GX2DrawdoneSync` off the same scene runs
> **33.27 ms / 30.06 fps**. See "Korok Forest is 20 fps instead of 30 because of one config default"
> below for the numbers; the 20.04 fps figure here is the *default-configuration* result and both are
> real.

**Nothing is saturated in the open world.** Full frame decomposition of Korok Forest, 49.90 ms
frame / 20.04 fps, from `--telemetry`:

| | ms/frame | % of frame |
|---|---|---|
| guest core 0 busy / idle | 7.87 – 8.14 / 41.96 – 42.18 | 16% / 84% |
| guest core 1 busy / idle | 9.39 – 9.47 / 39.64 – 39.72 | 19% / 79% |
| guest core 2 busy / idle | 9.48 – 9.62 / 37.46 – 37.55 | 19% / 75% |
| **all 3 guest cores busy** | **26.84 – 27.18** | **17.9 – 18.2% of 3-core capacity** |
| Latte thread: waiting for guest commands (`cp_idle`) | 19.47 – 19.51 | 39% |
| Latte thread: waiting for **vsync** (`cp_fence`) | 15.39 – 15.42 | 31% |
| GPU busy (asynchronous) | 17.12 – 17.17 † | 34% |

Guest cores ~18% busy, GPU ~34% busy, command processor ~70% waiting.

**This whole table is n=3 at `cebe17f` (2026-08-02) and every row reproduced** the n=1 values it
replaces to within 1.3%, the ranges above being the actual min–max across the three runs. It was
re-run because the original was single-sample and predated the phase split `491f4ef`; the conclusion
is unchanged, which is the point of saying so.

† `gpu.busy_ns` before `839466d` double-counted overlapping command buffers and read 19.20 ms here.
It is also the counter that soaks — see below — so quote it with a recording-window length attached.

**`cp_fence` is solved and is not a defect: it is the emulated vsync timer.** The fence is released
**10 µs** after a vsync signal (p99 30 µs), exactly one vsync and one flip fire per stall, and the
duration is the same in a 113-draw menu as in 3,516-draw gameplay. The guest thread that writes the
fence is parked in `GX2WaitForFlip`; the flip comes from `LatteTiming`'s polled software timer,
which the fence wait loop itself drives. That is frame pacing working. **Do not try to recover this
time** — and note it retroactively explains why parking the spin (`5933733`) and committing work
early both changed nothing: the guest was never waiting for the GPU. One flip per vsync also pins
`swapInterval = 1`, so the grid is 16.68 ms and a 49.90 ms frame is the title taking three slots.

**Korok Forest is 20 fps instead of 30 because of one config default, and it is not a renderer
problem.** `gpu.frame_critical_path_ns` (first `GetCommandBuffer()` of a frame → that frame's
presenting buffer leaving the GPU) is **35.25 ms**, which overruns the two-vsync-slot deadline of
33.27 ms and therefore takes three slots. Turning off **`GX2DrawdoneSync`** — a user-facing
checkbox, default on — drops the critical path to **18.64 ms**, the frame to **33.27 ms** and the
frame rate to **30.06 fps**, deterministic across n=3 control and n=2 treatment runs.

The cost is `GX2DrawDone`'s `IT_HLE_SYNC_ASYNC_OPERATIONS` packet, which BotW emits **twice per
frame** and whose handler force-finishes readbacks and queries with bare `waitUntilCompleted()` on
the Latte thread: **6.78 ms/frame, of which 6.75 ms is texture-readback force-finish**
(`gpu.occlusion_flush_ns` is 0.00). It collapses the intra-frame GPU gap from 16.39 ms to 0.25 ms.
(Re-measured at `cebe17f`, n=3: 6.96–7.00 ms and 6.93–6.98 ms. Same structure, and the difference is
the soak — this counter drifts 6.5 → 7.5 ms *within* a run.)

**Don't just flip the default** — it is a documented accuracy tradeoff, and the drain force-finishes
*every* in-flight readback whether or not the guest wants one. A surgical version *looked* like it
would keep the accuracy and recover most of the 6.75 ms; **it was tried three ways and does not
exist** — see "The readback drain is not surgically improvable" below before proposing one.

**Two Metal-side hypotheses are refuted, so don't re-raise them.** `queue_latency_ns`
(14.29 → 14.69 ms) and `command_buffers` (7 → 7) do not move between the 20 fps and 30 fps arms, so
neither the `MTLEvent` chain nor the commit threshold is what holds this scene back. Confirmed a
second way by the event-wrap fix below: buffers *had* been overlapping for most of every run and it
bought nothing.

**`gpu.busy_ns` before commit `839466d` is inflated — do not compare across that boundary.**
`EVENT_VALUE_WRAP` was 4096, so the signalled value went backwards every ~29 s and periodically
disabled the serialisation. `gpu.busy_ns` sums per-buffer intervals, so overlapping buffers were
double-counted. Fixed to a monotonic `uint64`; the same scene reads **16.8 ms** properly serialised
against 19.1–19.4 before, with frame time and critical path identical. The inflation grows with run
length (the first wrap is ~29 s in), so old figures are inconsistently wrong, not uniformly so.

The readback drain is **not** surgically improvable, measured three ways. `gpu.readback_forcestart`
is 0 and `gpu.readback_age_at_wait_ns` is 14.19 ms, so the 5-drawcall start delay holds nothing back
and the transfers we block on had already been running for ~7 ms each. `gpu.readback_draws_ahead` is
169/frame summed over all readbacks (~28 each) against 3,516 draws/frame, so the blit is not stuck
behind its own command buffer either. And **giving the blit a command buffer outside the `MTLEvent`
chain** — so it waited on neither the preceding buffer nor the ~500 draws in it — changed nothing at
n=3 per arm: frame 49.90 both, critical path 35.13–35.22 vs 35.10–35.23, force-finish 6.21–6.34 vs
6.14–6.27, GPU busy 16.44–16.51 vs 16.43–16.53. **Every performance range overlaps**; the only
separated metric was `gpu.command_buffers` 7 → 9, which merely proves the arm was live. The
mechanism was reverted (it rested on Metal's cross-command-buffer hazard tracking, unverified, for
zero payoff); only the counter and this finding were kept.

Note the control-vs-control pair from that batch: `readback_forcefinish_ns` swings **±2%** between
identical runs, while frame time, critical path and `queue_latency_ns` are stable to 0.0%. A "3%
improvement" in the drain counter is noise; do not report one without a same-arm control.

We are waiting for the GPU to catch up, which is what `GX2DrawDone` means. Reordering cannot help —
only doing less GPU work can.

**And "less GPU work" means fewer ALU and texture operations, not less bandwidth — measured.** A
`Metal GPU Counters` capture on Korok gameplay (179k samples, conditioned on the 42% of samples where
the GPU is active) puts **ALU as the top limiter in 45.6%** of them and texture sampling in 22.8%,
against **Last Level Cache 7.3% and MMU 5.5%**. Combined bandwidth is 41.26 GB/s mean. So the
804 MB/frame of attachment traffic is real but not limiting, and the planned in-pass streamout copy —
which would have removed 300 MB/frame of redundant tile store/reload — was **cancelled before being
built** because it relieves a subsystem that bottlenecks the GPU about an eighth of the time.
`Partial Renders Count` is 0 throughout, so there are no tile-memory spills either. See the master
plan; the next lever is shader cost, which is a different project.

Two numbers stand out as suspects:

- **52,330 HLE calls per frame** (1.05M/s) that cost the guest **nothing**. The `-= 500` at
  `BackendAArch64.cpp:874` applies *only* to the `0xFFD0` unresolved-import branch, which is 1.1% of
  calls (0.47% of one core). Every real HLE call — including `FSReadFile`, which on hardware is an
  IPC round trip to IOSU — advances guest time by zero cycles. That is a far larger timing
  divergence than a wrong flat charge would be, and it is why guest threads can issue a million
  library calls a second without consuming quantum.

  **A flat charge is now ruled out by measurement.** `cpu.hle_would_charge_cycles` sums a
  hypothetical 500/call without applying it: **25,103,500 cycles/frame against 15,384,240 actually
  retired, i.e. 163%** — it would more than double emulated guest time. The named histogram
  (`cpu.hle_calls` accuracy details, top 64) shows the volume is `OSFastMutex_Lock/Unlock`,
  `OSGetCoreId`, `OSBlockMove` and GX2 register setters, all worth tens of cycles on hardware, while
  the calls that matter are rare. **Any charge must be tiered per function.**
- **3,143 guest thread switches per frame** (63k/s), and the accounting closes:

  | reason | per frame | share |
  |---|---|---|
  | **voluntary `OSYieldThread`** | **2,496** | **79.4%** |
  | quantum exhausted | 330 | 10.5% |
  | blocked on a primitive | 313 | 10.0% |
  | unaccounted | 4 | 0.1% |

  Of the blocks: sleep 52%, message queue 24%, event 15%, semaphore 9%. Almost nothing waits on
  a mutex, and `GX2WaitForFlip` fires ~1/frame.

  **Four out of five context switches are the guest voluntarily yielding.** That is BotW polling,
  faithfully emulated — Cafe OS scheduling is cooperative (`docs/hardware/05`), so engine threads
  poll and yield rather than block. On console a yield is a cheap scheduler call. Here every one is
  a `ucontext` fiber switch, and `swapcontext` on Darwin/arm64 calls `sigprocmask` on both save and
  restore.

  **Now measured** — `tools/probes/fiber_switch_cost.c`, ping-pong 10⁶ switches: **454.5 ns** for
  `swapcontext` versus **6.5 ns** for the hand-written AArch64 switch, a 70x gap. At 3,143
  switches/frame that is **1.43 ms/frame today**, 2.9% of a 49.9 ms frame. The earlier ~700 ns
  estimate was ~35% high.

  **But it is not a frame-rate fix.** In gameplay the frame decomposes as **13.91 ms of work inside
  a 49.90 ms frame** quantised to three vsync periods, and that work already fits in *one* period
  with 16.6% headroom. Removing 1.4 ms from a stage with 35 ms of slack crosses no boundary. Same
  shape as the `DeviceShared` change: worth doing for power and headroom, not for fps.

  A microbenchmark is the right instrument and an in-process probe is not: `Fiber::Switch` does not
  return until the fiber is resumed, so a scope timer around it measures descheduled time — the trap
  that produced 197 ms of "busy" in a 49.9 ms frame.

`cpu.guest_cycles_retired` is reported but should not be read as a clean instruction count:
`__OSStoreThread` zeroes it when `executedCycles < skippedCycles`, so it undercounts.

**The renderer was holding uncommitted work while the CP blocked — and fixing it changed nothing.**
`MetalRenderer::NotifyLatteCommandProcessorIdle` held recorded drawcalls in **139,491 of 139,512**
idle notifications per gameplay frame. `--commit-on-fence-stall` (off by default) submits at stall
entry: work-held-at-idle drops **85%** and **nothing else moves** — frame time, fps, `cp_fence` and
GPU busy all flat to within 0.6%. The guest's 15.4 ms wait is not for GPU work we were withholding.
**Don't re-raise this.**

*(An earlier revision credited this change with −6.2% GPU busy. That was a menu/gameplay blend
artifact — see "A BotW telemetry run is two workloads" above — and on gameplay frames it is +0.5%, i.e. nothing.)*

Note the notification fires from two sites that look identical to the CP and are nothing alike:
`RingStarvation` ~129,000×/frame inside a park-and-recheck loop, `FenceStall` once. Committing on
the first would mint a command buffer per spin. `NotifyLatteCommandProcessorIdle` takes a reason
now for exactly that reason.

Already struck off by measurement, so don't re-raise them: `IT_MEM_SEMAPHORE` and the wait-for-flip
spin are **exactly zero** in BotW, and snapshotting guest threads at fence-stall entry proved
nothing — it found the cores idle 95% of the time, which is just their 96% baseline idle rate. A
probe with no control sample cannot discriminate.

**Do not divide BotW's GPU time by 16.67 ms.** BotW targets **30 FPS**, so the budget is 33.3 ms. An earlier revision of this file divided by the 60 FPS budget and concluded the GPU was at "108–147% of budget" and "is what caps the frame rate" — both wrong. At 15.6–18.5 ms against a ~35 ms wall-clock frame, the GPU sits at roughly **50% duty cycle and is not the limiter in this scene**: cutting GPU time 16% moved the frame rate not at all. Check what the title actually targets before computing a percentage.

### Driving a game without a controller (needed for the above)

`controllerProfiles/` ships empty, which is why input looks broken out of the box. Write `~/Library/Application Support/TesseraEmu/controllerProfiles/controller0.xml` directly (the GUI combo boxes don't respond to accessibility scripting): `<type>Wii U GamePad</type>`, `<api>Keyboard</api>`, `<uuid>keyboard</uuid>`, and `<mappings>` of `VPADController::ButtonId` → **macOS virtual key code** (`fix_raw_keycode` is a pass-through outside Windows). Then `osascript -e 'tell application "System Events" to key code 6'` presses that button, and `key down "w"` / `key up "w"` hold it. That is enough to script BotW's whole intro — no save file and no human needed.

For Metal work: `MTL_HUD_ENABLED=1` for a frame-time overlay, `MTL_DEBUG_LAYER=1` + `MTL_SHADER_VALIDATION=1` for validation, and the in-app Debug menu has GPU capture wired to `MTL::CaptureManager`.

## Architecture worth knowing before editing

**Guest CPU.** PPC → IML (an SSA-ish IR) → AArch64, in `src/Cafe/HW/Espresso/Recompiler/`. `PPCRecompilerImlGen*.cpp` lowers PPC to IML; `IML/IMLOptimizer.cpp` runs the passes; `BackendAArch64/` emits code via the `xbyak_aarch64` submodule. There is currently **no backend peephole pass** — the only one that existed was x86-specific and was deleted. The `IMLUtil_*` helpers in `IMLOptimizer.cpp` were kept as substrate for an AArch64 replacement.

`BackendAArch64.cpp` has 14 `static_assert`s pinning `PPCInterpreter_t` field offsets into AArch64's scaled-imm12 addressing range. **Any field added to or removed from `PPCState.h` is checked by these at compile time** — that is by design, not an obstacle.

Guest threads are `ucontext` fibers (`src/util/Fiber/FiberUnix.cpp`), not host threads. `makecontext` passes `int` arguments, so the 64-bit fiber parameter is split across two — hence `__OSFiberThreadEntry(uint32 _high, uint32 _low)`.

**Guest memory** is pure fastmem: one 4 GB `PROT_NONE` reservation, sub-ranges `mprotect`'d on demand, guest→host is `memory_base + addr` with no bounds check. Unmapped accesses fault to SIGSEGV. **Apple Silicon uses 16 KB pages, and both places that assumed 4 KB are fixed** — `MemMapperUnix.cpp:36-47` derives the page size from `getpagesize()` and widens every range outward before `mprotect`, and every `MMURange` entry in `MMU.cpp:113-126` is 16 KB-aligned in base *and* size (`CORE0/1/2_LC` rounded up from `0x5000`, `HIGHMEM` widened down from `0xFFFFF000` to the containing page). An earlier revision of this file said these "still contain 4 KB assumptions" long after Stage 3 landed the fix; see `02-cpu-jit-memory.md` §4.3 for what the bugs actually were.

**Graphics.** `LatteThread` is the single GPU command-processor thread; all renderer calls happen there. Latte (R700) shaders are decompiled to **MSL source text** and compiled at runtime. There is a persistent *pipeline* cache (`shaderCache/transferable/{titleid}_mtlpipeline.bin`, `MetalPipelineCache.cpp:267`), but it stores pipeline **state descriptors**, not compiled binaries — it replays them on the loading screen, so **every shader still recompiles from MSL on every launch**. The `MTLBinaryArchive` code that would fix this exists in `MetalPipelineCompiler.cpp` but is entirely commented out. The Metal backend keeps exactly one encoder alive at a time, so any mid-frame texture/buffer upload tears down the render pass.

**Threading.** ~45-47 threads on a 4P+4E machine. QoS **is** wired up now: `SetThreadName(name, ThreadRole)` (`util/helpers/helpers.cpp:121`) maps a role to a QoS class and applies it to the calling thread — guest cores and the GPU command thread `USER_INTERACTIVE`, input `USER_INITIATED`, compile/background `UTILITY` (deliberately not `BACKGROUND`, which macOS throttles hard). 16 call sites pass a role; anything still calling the one-argument form gets `UNSPECIFIED`. `g_CPUFeatures` exposes `performanceCores`/`efficiencyCores` for sizing worker pools, because `hardware_concurrency()` reports 8 and oversubscribes the shader compiler pools. `FSpinlock` is **no longer a spin** — it is `os_unfair_lock` (`util/helpers/fspinlock.h`), which donates priority; the old warning about QoS-before-`os_unfair_lock` producing a hang is discharged, both halves are done.

## Style

`.clang-format` exists; don't reformat whole files. From `CODING_STYLE.md`: `m_` for members, `s_` for statics, camelCase variables / PascalCase functions and types, braces on their own line. **Use Cemu's fixed-width types** (`uint32`, `sint32`, `uint64`, …) rather than `uint32_t` or `int`.

## Editing traps hit repeatedly in this codebase

- **Deleting a backend exposes hidden coupling that only the linker finds.** The MSL emitter forward-declared four helpers that were *defined in the GLSL emitter's* translation unit; several files received headers (`robin_hood`, `StringBuf`) only transitively through Vulkan/Zir includes.
- **Scripted edits fail silently.** Source uses tabs; a heredoc written with spaces will no-op a Python `str.replace` and report success. Regex-based block removal is worse — a greedy pattern once deleted 474 lines including the NEON code it was meant to keep. Prefer explicit line ranges, and always `git diff --stat` before building.
- **Probe the pattern the code actually uses, not the one the docs describe.** Apple documents a one-`MAP_JIT`-region-per-process limit; it is not enforced on macOS 26 (4000 succeed under hardened runtime). A first probe that "confirmed" the limit was testing an RWX pattern xbyak never uses. Probes preserved in `tools/probes/`.

## MCP servers worth reaching for

- **sosumi** — Apple documentation. Use it to confirm macOS 26 API availability rather than relying on memory; it is how `MTLSamplerDescriptor.lodBias` was confirmed as new in 26.0 and how the MetalFX input requirements were checked.
- **context7** — third-party library docs (wxWidgets, SDL3, cubeb, vcpkg).
