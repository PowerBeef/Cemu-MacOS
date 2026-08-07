# tessera-playtest MCP (Phase 1)

Autonomous playtesting tools for TesseraEmu: launch, raise window, hold/press
GamePad-mapped keys, screenshots, and JSON scenarios.

Built on Phase 0 helpers in `testing/playtest/`.

## Setup

```sh
./tools/tessera-playtest/setup.sh
```

Grok project config (`.grok/config.toml`) registers this as MCP **`tessera-playtest`**.

## Requirements

- Built binary: `bin/TesseraEmu_relwithdebinfo`
- ROM under `Roms/` (or pass `rom=`)
- macOS **Accessibility** for Terminal/Grok (System Events keys)
- Emulator window **frontmost** before input (`playtest_raise`)

## Typical agent flow

1. `playtest_install_profile` (or rely on launch default)
2. `playtest_launch` with optional `extra_args="--telemetry …"` / `enable_gdbstub=true`
3. `playtest_raise`
4. `playtest_run_botw_load_save` or `playtest_run_scenario` / individual `playtest_hold`
5. `playtest_screenshot` / `playtest_get_title`
6. `playtest_stop`

## Tools

| Tool | Purpose |
|------|---------|
| `playtest_health` | Server + repo + session |
| `playtest_install_profile` | Install keyboard GamePad profile |
| `playtest_list_buttons` | Named buttons |
| `playtest_launch` | Start emu (default BotW .wux) |
| `playtest_stop` | SIGTERM/KILL |
| `playtest_status` | pid / title / alive |
| `playtest_raise` | Frontmost |
| `playtest_get_title` / `wait_title` | Observe |
| `playtest_press` / `hold` / `release` | Input |
| `playtest_sleep` | Timing |
| `playtest_screenshot` | `capture-scene.sh` → `testing/golden/` |
| `playtest_run_scenario` | JSON/YAML steps |
| `playtest_run_botw_load_save` | BotW title → load save |

## Notes

- Holds must be ≥ **150 ms** (one emulated frame sample).
- Button names: see `testing/profiles/BUTTON_MAP.md` (`A`, `STICK_L_UP`, …).
- Guest GDB is separate: use **cemu-re** MCP with `--enable-gdbstub`.
- Phase 2 will add in-process ScriptedController (analog, no focus requirement).
