---
name: tessera-measure
description: >
  TesseraEmu performance and accuracy measurement discipline. Use when the user
  mentions telemetry, A/B tests, BotW, Korok, fps, GPU busy, frame time, counter
  zeros, xcprof compare, "did this make it faster", or any optimization claim.
---

# Tessera measure

Before claiming a win or a zero:

1. Read **`.grok/rules/measurement.md`** (always-loaded trap list).
2. For BotW: use **phase-split** (`testing/telemetry-report.py` default analyses the
   longest/high-draw phase). Do not median menu+gameplay as one run.
3. A/B arms: **same duration**, same point in the run (GPU counters soak).
4. Re-run **cmake configure** before quoting a build hash (`-DEMULATOR_HASH` is configure-time).
5. Check **`sw_vers`** when comparing to stored controls (OS is not in telemetry headers).
6. Non-overlapping n=3 ranges kill claims; they do not bless tiny gaps — need a **magnitude** bar.
7. Counter **zero**: confirm an increment site exists; prefer a positive control on the same path.
8. Prefer **`xcprof`** over raw xctrace; `compare` is share-of-CPU, not absolute — use `cputime` deltas for magnitude.
9. Land results in **`docs/status/ledger.json`** with `measured` + `ref`, then
   `python3 docs/status/build-status.py`.

Do not re-raise experiments already **refuted** / **cancelled** on the status page without new evidence.
