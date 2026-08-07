"""Process state for the playtest MCP (one emu instance per server process)."""

from __future__ import annotations

import os
import signal
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


@dataclass
class EmuSession:
	pid: int = 0
	rom: str = ""
	started_at: float = 0.0
	extra_args: list[str] = field(default_factory=list)
	log_path: str = ""

	def alive(self) -> bool:
		if self.pid <= 0:
			return False
		try:
			os.kill(self.pid, 0)
			return True
		except OSError:
			return False


_session = EmuSession()


def get_session() -> EmuSession:
	return _session


def set_session(pid: int, rom: str = "", extra: list[str] | None = None, log_path: str = "") -> EmuSession:
	global _session
	_session = EmuSession(
		pid=pid,
		rom=rom,
		started_at=time.time(),
		extra_args=list(extra or []),
		log_path=log_path,
	)
	return _session


def clear_session() -> None:
	global _session
	_session = EmuSession()


def require_pid(pid: int = 0) -> int:
	"""Return active pid: explicit arg, else tracked session."""
	if pid and pid > 0:
		return pid
	s = get_session()
	if s.pid > 0 and s.alive():
		return s.pid
	if s.pid > 0:
		raise RuntimeError(f"tracked pid {s.pid} is not running; launch again")
	raise RuntimeError("no active emu session — call playtest_launch first or pass pid=")


def find_repo_root() -> Path:
	"""Walk up from this file / cwd to find TesseraEmu root (has AGENTS.md + testing/)."""
	here = Path(__file__).resolve()
	for p in [here, *here.parents]:
		if (p / "AGENTS.md").is_file() and (p / "testing" / "playtest").is_dir():
			return p
	cwd = Path.cwd().resolve()
	for p in [cwd, *cwd.parents]:
		if (p / "AGENTS.md").is_file() and (p / "testing" / "playtest").is_dir():
			return p
	raise RuntimeError("could not locate TesseraEmu repo root (AGENTS.md + testing/playtest)")


def ensure_playtest_on_path(repo: Path) -> None:
	import sys

	testing = str(repo / "testing")
	if testing not in sys.path:
		sys.path.insert(0, testing)


def stop_pid(pid: int, grace_s: float = 2.0) -> dict:
	if pid <= 0:
		return {"status": "no_pid"}
	try:
		os.kill(pid, signal.SIGTERM)
	except ProcessLookupError:
		clear_session()
		return {"status": "already_dead", "pid": pid}
	deadline = time.time() + grace_s
	while time.time() < deadline:
		try:
			os.kill(pid, 0)
			time.sleep(0.1)
		except OSError:
			clear_session()
			return {"status": "stopped", "pid": pid, "signal": "SIGTERM"}
	try:
		os.kill(pid, signal.SIGKILL)
	except ProcessLookupError:
		pass
	clear_session()
	return {"status": "killed", "pid": pid, "signal": "SIGKILL"}


def pgrep_emu() -> list[int]:
	try:
		out = subprocess.check_output(["pgrep", "-x", "TesseraEmu_relwithdebinfo"], text=True)
	except subprocess.CalledProcessError:
		return []
	return [int(x) for x in out.split() if x.strip().isdigit()]
