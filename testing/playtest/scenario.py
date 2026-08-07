"""Minimal YAML-like scenario runner (JSON or simple step list).

Phase 0 supports a small JSON format; YAML optional if PyYAML is installed.
"""

from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any

from . import input_os, window
from .profile import install_keyboard_profile


def _load(path: Path) -> dict[str, Any]:
	text = path.read_text()
	if path.suffix in {".yaml", ".yml"}:
		try:
			import yaml  # type: ignore
		except ImportError as e:
			raise RuntimeError("PyYAML required for .yaml scenarios: pip install pyyaml") from e
		return yaml.safe_load(text)
	return json.loads(text)


def run_steps(pid: int, steps: list[dict[str, Any]]) -> None:
	for step in steps:
		if not isinstance(step, dict) or len(step) != 1:
			raise ValueError(f"step must be a single-key object: {step!r}")
		op, arg = next(iter(step.items()))
		if op == "raise":
			window.raise_window(pid)
		elif op == "sleep":
			time.sleep(float(arg))
		elif op == "press":
			if isinstance(arg, dict):
				input_os.press(str(arg.get("button", arg.get("key"))), float(arg.get("ms", 150)) / 1000.0)
			else:
				input_os.press(str(arg))
		elif op == "hold":
			if not isinstance(arg, dict):
				raise ValueError("hold needs {button|key, ms|seconds}")
			btn = str(arg.get("button", arg.get("key")))
			if "seconds" in arg:
				secs = float(arg["seconds"])
			else:
				secs = float(arg.get("ms", 150)) / 1000.0
			input_os.hold(btn, secs)
		elif op == "wait_title":
			if not isinstance(arg, dict):
				raise ValueError("wait_title needs {contains, timeout_s?}")
			window.wait_title(
				pid,
				contains=str(arg["contains"]),
				timeout_s=float(arg.get("timeout_s", 120)),
			)
		else:
			raise ValueError(f"unknown step op {op!r}")


def run_scenario_file(path: Path, pid: int, *, install_profile: bool = True) -> None:
	data = _load(path)
	if install_profile:
		install_keyboard_profile()
	steps = data.get("steps") or []
	run_steps(pid, steps)
