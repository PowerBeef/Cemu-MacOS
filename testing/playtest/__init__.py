"""Autonomous playtest helpers for TesseraEmu (Phase 0).

OS-level keyboard injection via System Events, matched to
``testing/profiles/gamepad-keyboard.xml``. Holds must be long enough for one
emulated frame sample (~150 ms minimum).
"""

from .buttons import BUTTONS, MIN_HOLD_S, resolve_button
from .input_os import hold, press, release, key_down, key_up
from .window import raise_window, get_window_title, wait_title
from .profile import install_keyboard_profile

__all__ = [
	"BUTTONS",
	"MIN_HOLD_S",
	"resolve_button",
	"hold",
	"press",
	"release",
	"key_down",
	"key_up",
	"raise_window",
	"get_window_title",
	"wait_title",
	"install_keyboard_profile",
]
