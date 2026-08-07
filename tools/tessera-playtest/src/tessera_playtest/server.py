"""MCP server: autonomous TesseraEmu playtesting (Phase 1).

Tools wrap testing/playtest (OS keyboard injection + window helpers) and launch/stop
the emulator binary. Requires macOS Accessibility for System Events.

Typical flow:
  playtest_install_profile → playtest_launch → playtest_raise → playtest_hold / run_scenario
  → playtest_screenshot / playtest_get_title → playtest_stop
"""

from __future__ import annotations

import os
import shlex
import subprocess
import time
from pathlib import Path
from mcp.server.fastmcp import FastMCP

from .state import (
	clear_session,
	ensure_playtest_on_path,
	find_repo_root,
	get_session,
	pgrep_emu,
	require_pid,
	set_session,
	stop_pid,
)

mcp = FastMCP("tessera-playtest")

_REPO: Path | None = None


def _repo() -> Path:
	global _REPO
	if _REPO is None:
		_REPO = find_repo_root()
		ensure_playtest_on_path(_REPO)
	return _REPO


def _playtest():
	_repo()
	import playtest  # type: ignore  # noqa: WPS433 — testing/ on path

	return playtest


# ---------- lifecycle ----------


@mcp.tool()
def playtest_health() -> dict:
	"""MCP alive check. Does not require a running emulator."""
	repo = str(_repo())
	s = get_session()
	return {
		"status": "ok",
		"repo": repo,
		"session_pid": s.pid or None,
		"session_alive": s.alive() if s.pid else False,
		"binary_exists": (_repo() / "bin" / "TesseraEmu_relwithdebinfo").is_file(),
	}


@mcp.tool()
def playtest_install_profile() -> dict:
	"""Install testing/profiles/gamepad-keyboard.xml as controller0.xml."""
	pt = _playtest()
	dest = pt.install_keyboard_profile()
	return {"status": "installed", "path": str(dest)}


@mcp.tool()
def playtest_list_buttons() -> dict:
	"""Named GamePad buttons available to press/hold (see BUTTON_MAP.md)."""
	pt = _playtest()
	return {"buttons": sorted(pt.BUTTONS.keys()), "min_hold_s": pt.MIN_HOLD_S}


@mcp.tool()
def playtest_launch(
	rom: str = "",
	extra_args: str = "",
	boot_wait_s: float = 100.0,
	kill_existing: bool = True,
	install_profile: bool = True,
	enable_gdbstub: bool = False,
) -> dict:
	"""Launch TesseraEmu_relwithdebinfo with a ROM.

	rom: path relative to repo root or absolute. Empty uses the BotW .wux under Roms/
	if present. extra_args: shell-style string e.g. '--telemetry t.jsonl --verbose'.
	boot_wait_s: sleep after spawn before returning (shader compile). Default 100 like drive-botw.
	"""
	repo = _repo()
	pt = _playtest()
	if install_profile:
		pt.install_keyboard_profile()

	bin_path = repo / "bin" / "TesseraEmu_relwithdebinfo"
	if not bin_path.is_file():
		raise FileNotFoundError(f"missing binary: {bin_path}")

	if rom:
		rom_path = Path(rom)
		if not rom_path.is_absolute():
			rom_path = repo / rom
	else:
		cands = sorted((repo / "Roms").glob("*.wux")) if (repo / "Roms").is_dir() else []
		if not cands:
			raise FileNotFoundError("no rom= given and no Roms/*.wux found")
		rom_path = cands[0]
	if not rom_path.is_file():
		raise FileNotFoundError(rom_path)

	if kill_existing:
		for p in pgrep_emu():
			stop_pid(p)

	args = [str(bin_path), "-g", str(rom_path)]
	if enable_gdbstub:
		args.append("--enable-gdbstub")
	if extra_args.strip():
		args.extend(shlex.split(extra_args))

	log_dir = Path.home() / "Library/Application Support/TesseraEmu"
	log_dir.mkdir(parents=True, exist_ok=True)
	log_path = log_dir / "playtest-mcp.log"
	# truncate shared log.txt so OSReport/boot lines are fresh for waiters
	log_txt = log_dir / "log.txt"
	try:
		log_txt.write_text("")
	except OSError:
		pass

	with open(log_path, "w") as logf:
		proc = subprocess.Popen(
			args,
			cwd=str(repo),
			stdout=logf,
			stderr=subprocess.STDOUT,
			start_new_session=True,
		)

	pid = proc.pid
	# Brief settle so the binary registers, then sleep boot_wait for shaders/title.
	time.sleep(min(2.0, max(float(boot_wait_s), 0.5)))
	found = pgrep_emu()
	if found:
		pid = found[0]
	elif not _alive(pid):
		raise RuntimeError(f"emulator exited immediately; see {log_path}")

	wait = max(float(boot_wait_s) - 2.0, 0.0)
	if wait > 0:
		# Poll so we fail fast if the process dies mid-boot.
		deadline = time.time() + wait
		while time.time() < deadline:
			if not _alive(pid):
				found = pgrep_emu()
				if found:
					pid = found[0]
				else:
					raise RuntimeError(f"emulator exited during boot_wait; see {log_path}")
			time.sleep(min(2.0, max(deadline - time.time(), 0.1)))

	found = pgrep_emu()
	if found:
		pid = found[0]
	if not _alive(pid):
		raise RuntimeError(f"emulator not running after boot_wait; see {log_path}")

	set_session(pid, rom=str(rom_path), extra=args[3:], log_path=str(log_path))
	return {
		"status": "launched",
		"pid": pid,
		"rom": str(rom_path),
		"args": args,
		"log": str(log_path),
		"boot_wait_s": boot_wait_s,
	}


def _alive(pid: int) -> bool:
	try:
		os.kill(pid, 0)
		return True
	except OSError:
		return False


@mcp.tool()
def playtest_stop(pid: int = 0) -> dict:
	"""Stop the emulator (tracked session or explicit pid)."""
	try:
		p = require_pid(pid)
	except RuntimeError as e:
		# try any running binary
		found = pgrep_emu()
		if not found:
			return {"status": "nothing_to_stop", "error": str(e)}
		p = found[0]
	return stop_pid(p)


@mcp.tool()
def playtest_status(pid: int = 0) -> dict:
	"""Pid, alive flag, window title if available."""
	s = get_session()
	try:
		p = require_pid(pid)
	except RuntimeError:
		found = pgrep_emu()
		return {
			"tracked_pid": s.pid or None,
			"running_pids": found,
			"alive": False,
			"title": None,
		}
	title = None
	try:
		title = _playtest().get_window_title(p)
	except Exception as e:
		title = f"<title error: {e}>"
	return {
		"pid": p,
		"alive": _alive(p),
		"title": title,
		"rom": s.rom if s.pid == p else None,
		"log": s.log_path or None,
	}


# ---------- input / window ----------


@mcp.tool()
def playtest_raise(pid: int = 0) -> dict:
	"""Bring TesseraEmu window to front (required before OS key injection)."""
	p = require_pid(pid)
	_playtest().raise_window(p)
	return {"status": "raised", "pid": p}


@mcp.tool()
def playtest_get_title(pid: int = 0) -> dict:
	"""Read the main window title (includes FPS when running)."""
	p = require_pid(pid)
	title = _playtest().get_window_title(p)
	return {"pid": p, "title": title}


@mcp.tool()
def playtest_wait_title(contains: str = "FPS:", timeout_s: float = 120.0, pid: int = 0) -> dict:
	"""Poll until window title contains a substring (default waits for FPS:)."""
	p = require_pid(pid)
	title = _playtest().wait_title(p, contains=contains, timeout_s=timeout_s)
	return {"pid": p, "title": title, "contains": contains}


@mcp.tool()
def playtest_press(button: str = "A", hold_ms: float = 150.0, pid: int = 0, raise_first: bool = True) -> dict:
	"""Press a named GamePad button (or single char). hold_ms default 150."""
	p = require_pid(pid)
	pt = _playtest()
	if raise_first:
		pt.raise_window(p)
		time.sleep(0.2)
	secs = max(hold_ms / 1000.0, pt.MIN_HOLD_S)
	pt.hold(button, secs)
	return {"status": "pressed", "button": button, "hold_ms": secs * 1000, "pid": p}


@mcp.tool()
def playtest_hold(
	button: str = "STICK_L_UP",
	seconds: float = 1.0,
	pid: int = 0,
	raise_first: bool = True,
) -> dict:
	"""Hold a named button/stick direction for ``seconds`` (walk, charge, etc.)."""
	p = require_pid(pid)
	pt = _playtest()
	if raise_first:
		pt.raise_window(p)
		time.sleep(0.2)
	pt.hold(button, seconds)
	return {"status": "held", "button": button, "seconds": seconds, "pid": p}


@mcp.tool()
def playtest_release(button: str = "A", pid: int = 0) -> dict:
	"""Release a char-based key if stuck down."""
	p = require_pid(pid)
	_playtest().release(button)
	return {"status": "released", "button": button, "pid": p}


@mcp.tool()
def playtest_sleep(seconds: float = 1.0) -> dict:
	"""Sleep helper for scenario-style agent loops."""
	time.sleep(max(0.0, float(seconds)))
	return {"status": "slept", "seconds": seconds}


# ---------- observe ----------


@mcp.tool()
def playtest_screenshot(name: str = "playtest", pid: int = 0, raise_first: bool = True) -> dict:
	"""Capture the Tessera window region to testing/golden/<name>.png (via capture-scene.sh)."""
	p = require_pid(pid)
	repo = _repo()
	if raise_first:
		_playtest().raise_window(p)
		time.sleep(0.3)
	script = repo / "testing" / "capture-scene.sh"
	if not script.is_file():
		raise FileNotFoundError(script)
	# sanitize name
	safe = "".join(c if c.isalnum() or c in "-_" else "_" for c in name) or "playtest"
	r = subprocess.run(
		["bash", str(script), str(p), safe],
		cwd=str(repo),
		capture_output=True,
		text=True,
	)
	out_png = repo / "testing" / "golden" / f"{safe}.png"
	return {
		"status": "ok" if r.returncode == 0 and out_png.is_file() else "error",
		"pid": p,
		"path": str(out_png) if out_png.is_file() else None,
		"returncode": r.returncode,
		"stdout": (r.stdout or "")[-500:],
		"stderr": (r.stderr or "")[-500:],
	}


@mcp.tool()
def playtest_run_scenario(
	path: str = "testing/scenarios/botw-load-save.json",
	pid: int = 0,
	raise_first: bool = True,
	install_profile: bool = True,
) -> dict:
	"""Run a JSON/YAML scenario file (steps: raise, sleep, press, hold, wait_title)."""
	repo = _repo()
	p = require_pid(pid)
	sc_path = Path(path)
	if not sc_path.is_absolute():
		sc_path = repo / path
	if not sc_path.is_file():
		raise FileNotFoundError(sc_path)
	pt = _playtest()
	if install_profile:
		pt.install_keyboard_profile()
	if raise_first:
		pt.raise_window(p)
		time.sleep(0.3)
	from playtest.scenario import run_scenario_file  # type: ignore

	run_scenario_file(sc_path, p, install_profile=False)
	title = None
	try:
		title = pt.get_window_title(p)
	except Exception:
		pass
	return {"status": "scenario_done", "path": str(sc_path), "pid": p, "title": title}


@mcp.tool()
def playtest_run_botw_load_save(
	settle_s: float = 0.0,
	pid: int = 0,
	boot_if_needed: bool = False,
	boot_wait_s: float = 100.0,
) -> dict:
	"""Convenience: BotW title → most recent save (same as drive-botw input sequence).

	If boot_if_needed and no session, launches default BotW rom with boot_wait_s first.
	Optional settle_s after the sequence (shader compile).
	"""
	repo = _repo()
	try:
		p = require_pid(pid)
	except RuntimeError:
		if not boot_if_needed:
			raise
		launch = playtest_launch(boot_wait_s=boot_wait_s, install_profile=True)
		p = int(launch["pid"])

	sc = repo / "testing" / "scenarios" / "botw-load-save.json"
	result = playtest_run_scenario(path=str(sc), pid=p, raise_first=True, install_profile=True)
	if settle_s > 0:
		time.sleep(float(settle_s))
	result["settle_s"] = settle_s
	try:
		result["title"] = _playtest().get_window_title(p)
	except Exception:
		pass
	return result


def main() -> None:
	mcp.run()


if __name__ == "__main__":
	main()
