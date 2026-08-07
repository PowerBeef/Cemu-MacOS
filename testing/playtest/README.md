# Playtest helpers (Phase 0)

Shared building blocks for **unattended** TesseraEmu input and observation.

## Layout

| Path | Role |
|------|------|
| `testing/profiles/gamepad-keyboard.xml` | Keyboard → GamePad profile |
| `testing/profiles/BUTTON_MAP.md` | VPAD id / keycode / character table |
| `testing/playtest/` | Python package: install profile, hold/press, raise window |
| `testing/scenarios/` | JSON step lists (e.g. BotW load save) |

## Requirements

- macOS **Accessibility** for Terminal/Grok (System Events key injection)
- Tessera window **frontmost** before keys (call `raise_window`)
- Holds ≥ **150 ms** so one emulated frame samples the key

## CLI examples

```sh
# Install profile only
python3 -c "from testing.playtest import install_keyboard_profile; print(install_keyboard_profile())"

# After Tessera is running with pid $PID:
python3 - <<PY
from testing.playtest import raise_window, press, hold, wait_title
pid = int(open('/tmp/tessera.pid').read())
raise_window(pid)
press("A")
hold("STICK_L_UP", 2.0)
print(wait_title(pid, contains="FPS:", timeout_s=60))
PY
```

## drive-botw.sh

Still the supported one-shot BotW boot. It installs the shared profile and uses the
same hold rules. Prefer extending scenarios here rather than more one-off shell.

## Phase 1 MCP

Project MCP **`tessera-playtest`** (`tools/tessera-playtest/`) wraps this package:

```sh
./tools/tessera-playtest/setup.sh
# Grok: .grok/config.toml → tessera-playtest
```

Tools: `playtest_launch`, `playtest_raise`, `playtest_hold`/`press`, `playtest_screenshot`,
`playtest_run_scenario`, `playtest_run_botw_load_save`, `playtest_stop`, …

Phase 2: in-emulator ScriptedController for focus-independent analog sticks.
