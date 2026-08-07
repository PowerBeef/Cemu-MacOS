"""Named GamePad buttons → injection tokens for the playtest keyboard profile.

Matches ``testing/profiles/gamepad-keyboard.xml`` and VPADController::ButtonId.
"""

from __future__ import annotations

from typing import Union

# name -> ("char", "z") or ("code", 126)
# Prefer char for face/stick (drive-botw proven path); key codes for arrows/specials.
ButtonSpec = tuple[str, Union[str, int]]

BUTTONS: dict[str, ButtonSpec] = {
	"A": ("char", "z"),
	"B": ("char", "x"),
	"X": ("char", "c"),
	"Y": ("char", "v"),
	"L": ("char", "q"),
	"R": ("char", "e"),
	"ZL": ("char", "r"),
	"ZR": ("char", "t"),
	"PLUS": ("code", 36),  # return
	"MINUS": ("code", 51),  # delete
	"UP": ("code", 126),
	"DOWN": ("code", 125),
	"LEFT": ("code", 123),
	"RIGHT": ("code", 124),
	"STICK_L_UP": ("char", "w"),
	"STICK_L_DOWN": ("char", "s"),
	"STICK_L_LEFT": ("char", "a"),
	"STICK_L_RIGHT": ("char", "d"),
	"STICK_R_UP": ("char", "i"),
	"STICK_R_DOWN": ("char", "k"),
	"STICK_R_LEFT": ("char", "j"),
	"STICK_R_RIGHT": ("char", "l"),
	# aliases used in scenarios / chat
	"+": ("code", 36),
	"-": ("code", 51),
	"START": ("code", 36),
	"SELECT": ("code", 51),
}

# Minimum hold so ≥1 emulated frame samples the key at 30 fps (see drive-botw.sh).
MIN_HOLD_S = 0.15


def resolve_button(name: str) -> ButtonSpec:
	raw = name.strip()
	key = raw.upper().replace(" ", "_")
	# Named GamePad buttons first (including single-letter A/B/X/Y/L/R).
	if key in BUTTONS:
		return BUTTONS[key]
	# Lowercase single-char passthrough for raw keys already in the profile ("z", "w").
	if len(raw) == 1 and raw.isalpha():
		return ("char", raw.lower())
	raise KeyError(f"unknown button {name!r}; known: {sorted(BUTTONS)}")
