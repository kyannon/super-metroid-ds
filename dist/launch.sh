#!/bin/bash
# Super Metroid DS launcher for ROCKNIX.
#   launch.sh                      play (the ROM next to this script, or in /storage/roms/snes)
#   launch.sh /path/game.sfc       play that ROM
#   (any --option=value from sm-ds.cfg can be appended)
# Output goes to sm-ds.log next to this script.

. /etc/profile >/dev/null 2>&1

DIR="$(cd "$(dirname "$0")" && pwd)"

# Controller mappings shipped with ROCKNIX
for db in /storage/.config/SDL-GameControllerDB/gamecontrollerdb.txt \
          /usr/config/SDL-GameControllerDB/gamecontrollerdb.txt; do
	if [ -f "$db" ]; then
		export SDL_GAMECONTROLLERCONFIG_FILE="$db"
		break
	fi
done

# Find sway's IPC socket so the app can span both screens.
if [ -z "$SWAYSOCK" ]; then
	for s in "$XDG_RUNTIME_DIR"/sway-ipc.*.sock /var/run/0-runtime-dir/sway-ipc.*.sock /run/user/*/sway-ipc.*.sock; do
		if [ -S "$s" ]; then
			export SWAYSOCK="$s"
			break
		fi
	done
fi

cd "$DIR"

# Second screen on and in place for the run (stock ROCKNIX keeps it off),
# put back afterwards; see screens.sh. The trap also covers being killed.
"$DIR/screens.sh" on > "$DIR/sm-ds.log" 2>&1
trap '"$DIR/screens.sh" off >> "$DIR/sm-ds.log" 2>&1' EXIT
trap 'exit 143' TERM INT HUP

./sm-ds "$@" >> "$DIR/sm-ds.log" 2>&1
