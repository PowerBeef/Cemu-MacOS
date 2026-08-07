"""Window focus and title polling via Accessibility / System Events."""

from __future__ import annotations

import subprocess
import time


def _osascript(source: str) -> str:
	r = subprocess.run(
		["osascript", "-e", source],
		check=False,
		capture_output=True,
		text=True,
	)
	return (r.stdout or "").strip()


def raise_window(pid: int) -> None:
	_osascript(
		f'tell application "System Events" to set frontmost of '
		f'(first process whose unix id is {int(pid)}) to true'
	)


def get_window_title(pid: int) -> str:
	return _osascript(
		f'tell application "System Events" to tell '
		f'(first process whose unix id is {int(pid)}) to '
		f'get value of attribute "AXTitle" of '
		f'(first window whose subrole is "AXStandardWindow")'
	)


def wait_title(
	pid: int,
	*,
	contains: str | None = None,
	timeout_s: float = 120.0,
	poll_s: float = 1.0,
) -> str:
	"""Poll window title until ``contains`` appears or timeout. Returns last title."""
	deadline = time.monotonic() + timeout_s
	last = ""
	while time.monotonic() < deadline:
		last = get_window_title(pid)
		if contains is None or (contains in last):
			return last
		time.sleep(poll_s)
	raise TimeoutError(f"title did not contain {contains!r} within {timeout_s}s; last={last!r}")
