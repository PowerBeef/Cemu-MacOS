# Rule: tools for TesseraEmu

How to pick tools on this repo. Prefer this matrix over habit or Axiom iOS defaults.

## Session start

```sh
cd <TesseraEmu-git-root>    # open Grok from the repo root
grok inspect                # after rule/MCP config changes: expect AGENTS.md + .grok/rules/*
grok mcp list               # cemu-re should show (project)
```

Health (host):

```sh
test -x /opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc && echo dkp-ok
which xcprof xcsym cmake ninja
python3 docs/status/build-status.py --verify | head -40
test -x tools/cemu-re-mcp/.venv/bin/python || ./tools/cemu-re-mcp/setup.sh
```

First user message should name the **goal**, the **porting/testing doc** to read, and the **success gate**
(boot title, ppc750cl report, telemetry n=3, `--verify` clean).

## Required context (always loaded)

| Path | Role |
|------|------|
| `AGENTS.md` | Constraints, build, architecture, editing traps |
| `.grok/rules/status-tracker.md` | Ledger obligation |
| `.grok/rules/measurement.md` | A/B and counter traps |
| `.grok/rules/tooling.md` | This file |

Deep design is **not** auto-injected: before touching a subsystem, read `docs/porting/0N-*.md` and
check `docs/status` for related entries.

## MCP

### Required user-scope (stop and ask if missing for the task)

| Server | Use when |
|--------|----------|
| **sosumi** | Any Metal / macOS 26 / Apple API question — do not invent availability |
| **context7** | wxWidgets, SDL3, cubeb, vcpkg, other third-party libs |
| **github** | PRs, CI, issues on this remote |

These are not in project config (account-level). If disconnected, say so and ask the user to reconnect
rather than guessing.

### Project-scope

| Server | Use when | Do not use when |
|--------|----------|-----------------|
| **cemu-re** | Guest PowerPC RE: mem/reg/BP under GDB | Metal, telemetry A/B, pure docs |

**cemu-re protocol:**

1. Launch: `./bin/TesseraEmu_relwithdebinfo --enable-gdbstub …` (port **1337**).
2. `connect_tool` → **`resume_tool` first** (stub halts at splash).
3. Soft BPs preferred; **hardware watchpoints may be stubbed on arm64 macOS**.
4. Disconnect kills the one-shot listener — hold the MCP process for the whole run.
5. Windows **pymem** tools are unavailable here (`pymem_status_tool`); use GDB path only.
6. Setup: `tools/cemu-re-mcp/README.md`, `./tools/cemu-re-mcp/setup.sh`.

## Host CLI (prefer over MCP when both exist)

| Task | Tool | Notes |
|------|------|-------|
| Build | `cmake` + Ninja | **Not** XcodeBuildMCP — this is not an Xcode app project |
| Profile / compare | **`xcprof`** | Prefer over raw `xctrace`; `compare` is share-of-CPU, not absolute |
| Symbolicate | `xcsym` | `.ips` / MetricKit |
| Test ROMs | `DEVKITPRO=/opt/devkitpro` | Official `dkp-pacman -S wiiu-dev` only; see `testing/toolchain/README.md` |
| Status | `python3 docs/status/build-status.py` | After ledger edits; `--verify` before push |
| Scene capture | `testing/capture-scene.sh` | Raise window first; needs keys + ROM |

## Playtest / character control

| Path | Role |
|------|------|
| `testing/profiles/gamepad-keyboard.xml` | Keyboard → GamePad profile for automation |
| `testing/profiles/BUTTON_MAP.md` | VPAD ButtonId ↔ keycode ↔ character |
| `testing/playtest/` | Python helpers used by MCP and scripts |
| `testing/scenarios/` | JSON step lists (e.g. `botw-load-save.json`) |
| `testing/drive-botw.sh` | One-shot BotW boot to most recent save |
| **MCP `tessera-playtest`** | Phase 1: launch / raise / hold / screenshot / scenarios |

Setup: `./tools/tessera-playtest/setup.sh`. Flow: `playtest_launch` → `playtest_raise` →
`playtest_run_botw_load_save` or `playtest_hold` → `playtest_screenshot` → `playtest_stop`.

**Rules:** Accessibility on for Terminal/Grok; window **frontmost** before keys; holds ≥ **150 ms**.
Do not invent keycodes — use the button map / `playtest_list_buttons`. Phase 2: in-emulator
ScriptedController for focus-independent analog sticks.

## Anti-patterns

- Axiom **SwiftUI / iOS / SpriteKit** auditors for this **C++20 Metal** emulator.
- Guessing **macOS 26** Metal APIs without **sosumi**.
- Quoting BotW fps/GPU without **phase-split** (see measurement rule).
- Trusting a counter **zero** without an increment site (see measurement rule).
- Reintroducing **`CLAUDE.md`** next to `AGENTS.md` (double-loads context).
- Building test ROMs via the deleted from-source fallback; official install only.
- Scripted **taps** shorter than one emulated frame (use hold ≥ 150 ms).
