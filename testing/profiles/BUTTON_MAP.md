# Playtest keyboard → GamePad map

Profile: `gamepad-keyboard.xml` → install as
`~/Library/Application Support/TesseraEmu/controllerProfiles/controller0.xml`.

On macOS, Tessera samples **virtual key codes** (not characters). Scripted input must
**hold** ≥ ~150 ms so at least one emulated frame samples the key (30 fps ≈ 33 ms).

| VPAD `ButtonId` | Name | Keycode | US key / script char |
|----------------:|------|--------:|----------------------|
| 1 | A | 6 | z |
| 2 | B | 7 | x |
| 3 | X | 8 | c |
| 4 | Y | 9 | v |
| 5 | L | 12 | q |
| 6 | R | 14 | e |
| 7 | ZL | 15 | r |
| 8 | ZR | 17 | t |
| 9 | + | 36 | return |
| 10 | − | 51 | delete |
| 11 | D-pad up | 126 | ↑ |
| 12 | D-pad down | 125 | ↓ |
| 13 | D-pad left | 123 | ← |
| 14 | D-pad right | 124 | → |
| 17 | Stick L up | 13 | w |
| 18 | Stick L down | 1 | s |
| 19 | Stick L left | 0 | a |
| 20 | Stick L right | 2 | d |
| 21 | Stick R up | 34 | i |
| 22 | Stick R down | 40 | k |
| 23 | Stick R left | 38 | j |
| 24 | Stick R right | 37 | l |

Python helpers: `testing/playtest/` — `press("A")`, `hold("STICK_L_UP", 2.0)`.
