#!/bin/bash
# Gets both screens ready for Super Metroid DS under sway (ROCKNIX), and puts them back
# afterwards. Run by launch.sh:
#
#   screens.sh on    before Super Metroid DS starts
#   screens.sh off   after it exits (crashed or not)
#
# Stock ROCKNIX leaves the second screen of a dual-screen handheld powered
# off, with no fixed place in the layout, and its sway-touch service ties the
# touchscreen to the first screen. Super Metroid DS spans one window over both screens
# (sway "fullscreen global"), so "on":
#   - powers the second screen on (and enables it if it's disabled),
#   - puts it to the right of the first one, as ROCKNIX's touch calibration
#     for the RG DS and RG DS Plus expects (bottom-panel touches land in the
#     right half of the layout),
#   - on those two devices, maps the touchscreen to the whole layout so the
#     calibration places touches on the bottom screen.
# "off" restores the outputs' power, enabled state and positions as they were,
# and maps the touchscreen back to the first screen if ROCKNIX's sway-touch
# service is in charge of it (ROCKNIXDS turns that service off and keeps the
# touchscreen on the whole layout, so it's left alone there).
#
# Does nothing without sway, swaymsg, jq or a second screen.

STATE=/tmp/sm-ds-screens.state
log() { echo "screens: $*"; }

command -v swaymsg >/dev/null 2>&1 && command -v jq >/dev/null 2>&1 || exit 0
if [ -z "$SWAYSOCK" ]; then
	for s in "$XDG_RUNTIME_DIR"/sway-ipc.*.sock /var/run/0-runtime-dir/sway-ipc.*.sock /run/user/*/sway-ipc.*.sock; do
		[ -S "$s" ] && export SWAYSOCK="$s" && break
	done
fi
[ -n "$SWAYSOCK" ] || exit 0

sm() { swaymsg "$@" >/dev/null 2>&1 || log "swaymsg $* failed"; }

case "$1" in
on)
	rm -f "$STATE"
	# name active power x y width focused, one output per line
	OUTS=$(swaymsg -r -t get_outputs | jq -r '.[] | [.name, .active, (if .power == null then .dpms else .power end), .rect.x, .rect.y, .rect.width, .focused] | @tsv')
	[ -n "$OUTS" ] || exit 0
	# the first screen: the one ROCKNIX's sway set up as the main output, else the focused one
	TOP="$WLR_CON"
	if [ -z "$TOP" ] || ! printf '%s\n' "$OUTS" | cut -f1 | grep -qx "$TOP"; then
		TOP=$(printf '%s\n' "$OUTS" | awk -F'\t' '$7 == "true" { print $1; exit }')
	fi
	[ -n "$TOP" ] || TOP=$(printf '%s\n' "$OUTS" | head -n1 | cut -f1)
	# the second screen: another built-in panel (not HDMI / DP)
	SECOND=$(printf '%s\n' "$OUTS" | awk -F'\t' -v top="$TOP" '$1 != top && $1 ~ /^(DSI|eDP)/ { print $1; exit }')
	if [ -z "$SECOND" ]; then
		log "one screen ($TOP), nothing to do"
		exit 0
	fi
	TOPW=$(printf '%s\n' "$OUTS" | awk -F'\t' -v top="$TOP" '$1 == top { print $6 }')
	SECW=$(swaymsg -r -t get_outputs | jq -r --arg n "$SECOND" '.[] | select(.name == $n) | (.current_mode.width // .modes[0].width // 0)')
	[ "${TOPW:-0}" -gt 0 ] || TOPW=$(swaymsg -r -t get_outputs | jq -r --arg n "$TOP" '.[] | select(.name == $n) | (.current_mode.width // .modes[0].width // 0)')

	# what to put back afterwards
	{
		printf 'TOP=%s\nSECOND=%s\n' "$TOP" "$SECOND"
		printf '%s\n' "$OUTS" | awk -F'\t' -v top="$TOP" -v sec="$SECOND" '
			$1 == top || $1 == sec { printf "OUT\t%s\t%s\t%s\t%s\t%s\n", $1, $2, $3, $4, $5 }'
	} >"$STATE"

	log "before: $(printf '%s' "$OUTS" | tr '\t\n' ' ;')"
	sm "output $SECOND enable"
	sm "output $SECOND power on"
	sm "output $TOP pos 0 0"
	sm "output $SECOND pos ${TOPW:-0} 0"
	log "$TOP at 0,0 and $SECOND at ${TOPW:-0},0 (${SECW:-?} wide), powered on"
	log "after: $(swaymsg -r -t get_outputs | jq -r '.[] | [.name, .active, (if .power == null then .dpms else .power end), .rect.x, .rect.y, .rect.width, .rect.height] | @tsv' | tr '\t\n' ' ;')"

	case "$QUIRK_DEVICE" in
	"Anbernic RG DS" | "Anbernic RG DS Plus")
		swaymsg -r -t get_inputs | jq -r '.[] | select(.type == "touch") | .identifier' | while read -r t; do
			[ -n "$t" ] || continue
			sm "input \"$t\" map_to_output *"
			printf 'TOUCH\t%s\n' "$t" >>"$STATE"
			log "touchscreen $t on the whole layout"
		done
		# ROCKNIX's sway-touch service maps the touchscreen to the first
		# screen at boot; if it's in charge, put that back afterwards
		if systemctl cat sway-touch.service 2>/dev/null | grep -q '^ExecStart=/usr/bin/sway-touch.sh' &&
			! systemctl cat sway-touch.service 2>/dev/null | grep -q '^ExecStart=/bin/true'; then
			echo "TOUCH_BACK=1" >>"$STATE"
		fi
		;;
	esac
	;;
off)
	[ -f "$STATE" ] || exit 0
	TOP=$(sed -n 's/^TOP=//p' "$STATE")
	SECOND=$(sed -n 's/^SECOND=//p' "$STATE")
	grep '^OUT' "$STATE" | while IFS=$'\t' read -r _ name active power x y; do
		[ "$active" = "true" ] || continue
		sm "output $name pos $x $y"
	done
	grep '^OUT' "$STATE" | while IFS=$'\t' read -r _ name active power x y; do
		if [ "$active" != "true" ]; then
			sm "output $name disable"
		elif [ "$power" = "false" ]; then
			sm "output $name power off"
		fi
	done
	if grep -q '^TOUCH_BACK=1' "$STATE"; then
		grep '^TOUCH' "$STATE" | grep -v '^TOUCH_BACK' | cut -f2 | while read -r t; do
			sm "input \"$t\" map_to_output $TOP"
		done
	fi
	log "screens put back"
	rm -f "$STATE"
	;;
*)
	echo "usage: $0 on|off" >&2
	exit 1
	;;
esac
exit 0
