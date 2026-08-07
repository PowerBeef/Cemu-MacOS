"""Install the shared keyboard GamePad profile for automation."""

from __future__ import annotations

import shutil
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
_DEFAULT_PROFILE = _REPO / "testing" / "profiles" / "gamepad-keyboard.xml"


def install_keyboard_profile(
	profile: Path | None = None,
	*,
	cemu_dir: Path | None = None,
) -> Path:
	"""Copy profile to Application Support as controller0.xml. Returns dest path."""
	src = Path(profile) if profile else _DEFAULT_PROFILE
	if not src.is_file():
		raise FileNotFoundError(src)
	base = Path(cemu_dir) if cemu_dir else Path.home() / "Library/Application Support/TesseraEmu"
	dest_dir = base / "controllerProfiles"
	dest_dir.mkdir(parents=True, exist_ok=True)
	dest = dest_dir / "controller0.xml"
	shutil.copyfile(src, dest)
	return dest
