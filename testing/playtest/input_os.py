"""OS-level key injection via System Events (Accessibility required).

Tessera must be frontmost for keys to reach the wx keystate map. Always call
``raise_window(pid)`` before a sequence.
"""

from __future__ import annotations

import subprocess
import time
from typing import Union

from .buttons import MIN_HOLD_S, resolve_button


def _osascript(source: str) -> None:
	subprocess.run(
		["osascript", "-e", source],
		check=False,
		stdout=subprocess.DEVNULL,
		stderr=subprocess.DEVNULL,
	)


def key_down(spec: tuple[str, Union[str, int]]) -> None:
	kind, val = spec
	if kind == "char":
		_osascript(f'tell application "System Events" to key down "{val}"')
	elif kind == "code":
		# System Events has no durable "key down code"; use key code press for taps.
		# For holds of arrows we emit key code repeatedly — see hold().
		_osascript(f'tell application "System Events" to key code {int(val)}')
	else:
		raise ValueError(spec)


def key_up(spec: tuple[str, Union[str, int]]) -> None:
	kind, val = spec
	if kind == "char":
		_osascript(f'tell application "System Events" to key up "{val}"')
	elif kind == "code":
		pass  # key code is instantaneous
	else:
		raise ValueError(spec)


def press(button: str, hold_s: float = MIN_HOLD_S) -> None:
	"""Press and release a named button or single character."""
	hold(button, hold_s)


def hold(button: str, seconds: float) -> None:
	"""Hold a named button for ``seconds`` (clamped to at least MIN_HOLD_S)."""
	seconds = max(float(seconds), MIN_HOLD_S)
	spec = resolve_button(button)
	kind, val = spec
	if kind == "char":
		key_down(spec)
		time.sleep(seconds)
		key_up(spec)
		return
	# key codes: repeat short presses for the duration (digital hold approximation)
	deadline = time.monotonic() + seconds
	while time.monotonic() < deadline:
		_osascript(f'tell application "System Events" to key code {int(val)}')
		time.sleep(MIN_HOLD_S)


def release(button: str) -> None:
	"""Release a char-based button if stuck down."""
	spec = resolve_button(button)
	if spec[0] == "char":
		key_up(spec)
