# Wii U hardware & Cafe OS reference

A knowledge base for emulator **accuracy** and **performance** work on this fork. It exists so that
hardware semantics stop being re-researched per question, and so that the places where we knowingly
diverge from the machine are written down instead of rediscovered.

This is not a general Wii U encyclopaedia. It is exhaustive where a detail changes what the emulator
should do, and deliberately shallow where it does not (`08-io-peripherals.md` says so explicitly).

## Chapters

| | |
|---|---|
| [`01-espresso-cpu.md`](01-espresso-cpu.md) | Tri-core PowerPC 750CL derivative: cores, caches, paired singles, timebase, locked cache |
| [`02-memory-hierarchy.md`](02-memory-hierarchy.md) | MEM0/MEM1/MEM2, the full address map, tiling aperture, page sizes |
| [`03-latte-gpu.md`](03-latte-gpu.md) | GPU7/R700: register file, limits, tiling, formats |
| [`04-gx2-command-model.md`](04-gx2-command-model.md) | Write-gather → command pool → PM4 → TCL ring → command processor |
| [`05-cafeos-scheduling.md`](05-cafeos-scheduling.md) | Cooperative threading, priorities, affinity, and how we model it with fibers |
| [`06-cafeos-libraries.md`](06-cafeos-libraries.md) | The 46 RPLs, the HLE trap mechanism, per-library gaps |
| [`07-audio-dsp.md`](07-audio-dsp.md) | The Latte-resident DSP and the AX mixing model |
| [`08-io-peripherals.md`](08-io-peripherals.md) | IOSU, FS, DRC, Wiimote, USB — boundaries only |
| [`09-accuracy-gap-register.md`](09-accuracy-gap-register.md) | **Every known divergence, prioritised.** The payoff chapter |

Related, and assumed rather than repeated here: `../porting/00-master-plan.md` (staged plan and risk
register), `../porting/02-cpu-jit-memory.md` (recompiler internals), `../porting/03-graphics-metal.md`
(Metal backend), and `../../AGENTS.md` (build, measurement protocol, current baselines).

## Provenance tags

Sources on this hardware disagree, and a lot of what circulates is folklore repeated between forum
posts. Every non-obvious claim in these chapters carries a tag:

| Tag | Meaning |
|---|---|
| `[SRC]` | Verified in this repository, with `file:line`. The strongest claim available here — it is what the emulator actually does. |
| `[HW]` | Vendor documentation: IBM/AMD manuals, or the Cafe SDK release notes. |
| `[RE]` | Reverse-engineering consensus — WiiUBrew, fail0verflow, decaf-emu. Usually right, occasionally repeated without verification. |
| `[EST]` | Inference or estimate. Explicitly not verified. Do not build a decision on one of these without measuring first. |
| `[CONFLICT]` | Sources disagree. Both values are recorded and neither is asserted. |

**A `[SRC]` tag is a claim about this emulator, not about the hardware.** Where the two differ, that
is the point — the difference is an accuracy gap and belongs in chapter 09.

## The Modelled / Approximated / Absent table

Every chapter ends with one. It maps each hardware behaviour to one of:

- **Modelled** — we implement the observable behaviour.
- **Approximated** — we implement something close enough that no known title notices, and the
  chapter says *how* it differs.
- **Absent** — not implemented. Either nothing observes it, or it is a real gap (→ chapter 09).

These tables are the input to the telemetry harness's accuracy signals: an "Approximated" or
"Absent" row that a title actually hits at runtime is exactly what we want counted and named.

## Conventions

- Guest addresses are the **virtual** addresses a Cafe OS title sees. On this hardware, and in this
  emulator, virtual and physical are identity for everything a title touches `[SRC MMU.cpp:288-298]`.
- Guest data is **big-endian**. Where a struct is quoted, `betype<T>`/`uint32be` means the value is
  stored byte-swapped relative to the host.
- "Core" without qualification means an emulated Espresso core (0–2), keyed by `spr.UPIR`, not a
  host CPU core.
- Sizes are binary (`KB` = 1024 bytes).

## Verifying this document

Every `[SRC]` tag must resolve. `tools/docs/check-src-refs.py` greps each `file:line` reference and
fails if the file is gone or the line no longer plausibly matches — so these chapters cannot rot
silently as the code moves underneath them. Run it before trusting an old chapter.

## Bibliography

- WiiUBrew — [Hardware](https://wiiubrew.org/wiki/Hardware), [Memory map](https://wiiubrew.org/wiki/Memory_map),
  [Espresso](https://wiiubrew.org/wiki/Espresso), [Hardware/GX2](https://wiiubrew.org/wiki/Hardware/GX2),
  [Coreinit.rpl](https://wiiubrew.org/wiki/Coreinit.rpl)
- WiiBrew — [Hardware/Broadway](https://wiibrew.org/wiki/Hardware/Broadway), [Paired single](https://wiibrew.org/wiki/Paired_single)
- Rodrigo Copetti — [Wii U Architecture: A Practical Analysis](https://www.copetti.org/writings/consoles/wiiu/)
- IBM — *Gekko RISC Microprocessor User's Manual* v1.2; *A PowerPC compatible processor supporting
  high-performance 3-D graphics* (Hot Chips 13)
- Nintendo — Cafe SDK GX2 release notes (the single most useful `[HW]` source for GPU behaviour and
  documented hardware bugs)
- devkitPro [`wut`](https://github.com/devkitPro/wut) — `coreinit`/`gx2` headers; the authority on
  Cafe OS API semantics
- decaf-emu — [`wiiu-tests`](https://github.com/decaf-emu/wiiu-tests) hardware behaviour tests
