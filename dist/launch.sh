#!/bin/bash
# Super Metroid DS launcher for ROCKNIX and BananaOS.
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

# BananaOS (github.com/JorreFog/BananaLinux): no compositor and no sway.
# SMDS_BANANA_VIDEO picks how the app reaches the two panels:
#   kmsdrm (default): SDL's KMS/DRM driver, a window on each panel, drawn by
#     the GPU straight onto it (the top panel is display 0: SDL_KMSDRM_FIRST,
#     from Banana's SDL patch)
#   evdev: Banana's SDL patch puts one window over both panels
#     (SDL_BANANA_PANELS: the top one left), drawn by the CPU and copied onto
#     them each frame (much slower)
#   offscreen: the same with OpenGL ES, each frame read back (slower still)
# Banana's paths replace ROCKNIX's for the settings still at their defaults
# in sm-ds.cfg (anything you changed there is kept).
if [ -d /etc/banana ]; then
	LOG="$DIR/sm-ds.log"
	VIDEO=${SMDS_BANANA_VIDEO:-kmsdrm}
	echo "BananaOS, video: $VIDEO" > "$LOG"
	B=()
	cfgval() { sed -n "s/^$1 *= *//p" "$DIR/sm-ds.cfg" 2>/dev/null | tail -n 1; }
	if [ "$VIDEO" = kmsdrm ]; then
		export SDL_VIDEODRIVER=kmsdrm SDL_KMSDRM_FIRST=${BANANA_TOP:-DSI-2}
		unset SDL_BANANA_PANELS
		# a window on each panel; touches are the bottom panel's own
		case "$(cfgval layout)" in auto|"") B+=(--layout=dual) ;; esac
		case "$(cfgval touch_map)" in window|"") B+=(--touch-map=screen) ;; esac
	else
		export SDL_VIDEODRIVER=$VIDEO SDL_BANANA_PANELS=side SDL_BANANA_TOP=${BANANA_TOP:-DSI-2}
		unset SDL_KMSDRM_FIRST
	fi
	# the ports launcher switched the bottom panel's backlight off (and
	# bananad keeps it off while /run/banana/panel-off names it): on for us
	if [ -f /run/banana/panel-off ]; then
		BL=$(cat /run/banana/panel-off)
		rm -f /run/banana/panel-off
		[ -w "$BL/bl_power" ] && echo 0 > "$BL/bl_power"
	fi
	# Banana keeps SNES games in roms/snes and RetroArch's saves in /data/saves
	case "$(cfgval rom_dir)" in /storage/roms/snes*|"") B+=(--rom-dir=/data/roms/snes) ;; esac
	[ -z "$(cfgval save_dir)" ] && B+=(--save-dir=/data/saves)
	mkdir -p /data/saves 2>/dev/null
	echo "Banana paths: ${B[*]}" >> "$LOG"
	exec ./sm-ds "${B[@]}" "$@" >> "$LOG" 2>&1
fi

# Second screen on and in place for the run (stock ROCKNIX keeps it off),
# put back afterwards; see screens.sh. The trap also covers being killed.
"$DIR/screens.sh" on > "$DIR/sm-ds.log" 2>&1
trap '"$DIR/screens.sh" off >> "$DIR/sm-ds.log" 2>&1' EXIT
trap 'exit 143' TERM INT HUP

./sm-ds "$@" >> "$DIR/sm-ds.log" 2>&1
