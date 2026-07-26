#!/bin/bash
# Boot Breath of the Wild and drive it, unattended, to a fixed heavy scene:
# Link standing still in the Shrine of Resurrection.
#
#   ./testing/drive-botw.sh [seconds_to_settle]
#
# Prints the pid on success. That scene is the graphics measurement target for
# this fork -- it is GPU-bound (108-147% of a 16.67ms budget) and exactly
# repeatable (draws/frame holds at 4838.8 +/- 0.3), which no MK8 scene is.
#
# No controller, no save file and no human input required. This works because:
#   - controllerProfiles/ ships empty, so we write controller0.xml ourselves;
#     the GUI combo boxes do not respond to accessibility scripting, the file does.
#   - button values in that file are macOS virtual key codes, since
#     wxKeyEvent::GetRawKeyCode() is a pass-through on macOS (fix_raw_keycode's
#     fixups are all inside #if BOOST_OS_WINDOWS).
#   - osascript sends those same virtual key codes, so the two line up exactly.
set -euo pipefail

SETTLE="${1:-90}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CEMU_DIR="$HOME/Library/Application Support/Cemu"
ROM="$REPO/Roms/Legend of Zelda, The - Breath of the Wild (USA) (En,Fr,Es).wux"
BIN="$REPO/bin/Cemu_relwithdebinfo"

[ -f "$ROM" ] || { echo "missing ROM: $ROM" >&2; exit 1; }
[ -x "$BIN" ] || { echo "missing binary: $BIN (build first)" >&2; exit 1; }

# --- keyboard controller profile -------------------------------------------
# mapping ids are VPADController::ButtonId, button values are macOS virtual key codes
mkdir -p "$CEMU_DIR/controllerProfiles"
cat > "$CEMU_DIR/controllerProfiles/controller0.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<emulated_controller>
	<type>Wii U GamePad</type>
	<controller>
		<api>Keyboard</api>
		<uuid>keyboard</uuid>
		<display_name>Keyboard</display_name>
		<axis><deadzone>0.25</deadzone><range>1</range></axis>
		<rotation><deadzone>0.25</deadzone><range>1</range></rotation>
		<trigger><deadzone>0.25</deadzone><range>1</range></trigger>
		<mappings>
			<entry><mapping>1</mapping><button>6</button></entry>
			<entry><mapping>2</mapping><button>7</button></entry>
			<entry><mapping>3</mapping><button>8</button></entry>
			<entry><mapping>4</mapping><button>9</button></entry>
			<entry><mapping>5</mapping><button>12</button></entry>
			<entry><mapping>6</mapping><button>14</button></entry>
			<entry><mapping>7</mapping><button>15</button></entry>
			<entry><mapping>8</mapping><button>17</button></entry>
			<entry><mapping>9</mapping><button>36</button></entry>
			<entry><mapping>10</mapping><button>51</button></entry>
			<entry><mapping>11</mapping><button>126</button></entry>
			<entry><mapping>12</mapping><button>125</button></entry>
			<entry><mapping>13</mapping><button>123</button></entry>
			<entry><mapping>14</mapping><button>124</button></entry>
			<entry><mapping>17</mapping><button>13</button></entry>
			<entry><mapping>18</mapping><button>1</button></entry>
			<entry><mapping>19</mapping><button>0</button></entry>
			<entry><mapping>20</mapping><button>2</button></entry>
			<entry><mapping>21</mapping><button>34</button></entry>
			<entry><mapping>22</mapping><button>40</button></entry>
			<entry><mapping>23</mapping><button>38</button></entry>
			<entry><mapping>24</mapping><button>37</button></entry>
		</mappings>
	</controller>
</emulated_controller>
XML

pkill -f Cemu_relwithdebinfo 2>/dev/null || true
sleep 2
: > "$CEMU_DIR/log.txt"

nohup "$BIN" -g "$ROM" >/dev/null 2>&1 &
sleep 100   # boot + initial shader compilation

PID=$(pgrep -f Cemu_relwithdebinfo | head -1)
[ -n "$PID" ] || { echo "Cemu failed to start" >&2; exit 1; }

raise() { osascript -e "tell application \"System Events\" to set frontmost of (first process whose unix id is $PID) to true" >/dev/null 2>&1 || true; }

# Press a mapped button. Must HOLD it: Cemu samples keystate once per emulated
# frame (33ms at 30fps), so a bare `key code` tap is often shorter than one
# sample and silently does nothing. This was the single biggest time sink in
# getting scripted input to work.
press() {
  osascript -e "tell application \"System Events\" to key down \"$1\"" >/dev/null 2>&1 || true
  sleep 0.15
  osascript -e "tell application \"System Events\" to key up \"$1\"" >/dev/null 2>&1 || true
}
hold() {
  osascript -e "tell application \"System Events\" to key down \"$1\"" >/dev/null 2>&1 || true
  sleep "$2"
  osascript -e "tell application \"System Events\" to key up \"$1\"" >/dev/null 2>&1 || true
}

raise; sleep 2

# 'z' is the key mapped to the A button above.
# title screen -> main menu
press z; sleep 6
# main menu: "Continue" is the default selection when a save exists
press z; sleep 6
# save-slot list: the autosave is preselected
press z; sleep 10
# dismiss anything that follows
press z

echo "settling ${SETTLE}s for shader/pipeline compilation..." >&2
sleep "$SETTLE"

echo "$PID"
