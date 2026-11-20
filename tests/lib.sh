# tests/lib.sh - what run.sh, evil.sh, width.sh and headless.sh share: their
# directory and environment, a headless sway of their own with one config,
# vinput from mwc as the mouse, wtype as the keyboard, the window from sway's
# tree, the screen read through the primary selection. Sourced from the source
# tree; harness_dir sets DIR, sway_start SWAY, VIN_PID, WAYLAND_DISPLAY and
# SWAYSOCK, harness_cleanup ends them.

# Where sway puts the window, and config.h's padding: the cell under a point.
MWT_X=20 MWT_Y=20
PAD=$(sed -n 's/^static const int padding = \([0-9]*\);.*/\1/p' config.h)

# harness_dir <name>: a directory of its own, as XDG_RUNTIME_DIR and HOME, with
# mwt.info compiled into it; nothing of the session the tests run in.
harness_dir() {
	DIR=$(mktemp -d "/tmp/mwt-$1.XXXXXX") || exit 1
	mkdir -p "$DIR/home" "$DIR/config"
	export DIR XDG_RUNTIME_DIR=$DIR HOME=$DIR/home XDG_CONFIG_HOME=$DIR/config
	export SHELL=${HARNESS_SHELL:-/bin/bash}
	export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
	export LANG=C.UTF-8 LC_ALL=C.UTF-8
	unset WAYLAND_DISPLAY DISPLAY TERMINFO TERMINFO_DIRS MWC_ACTIVE MWC_INACTIVE TMUX
	tic -x -o "$HOME/.terminfo" mwt.info || { echo "FAIL: mwt.info does not compile"; exit 1; }
}

# sway_conf [line...]: a 1000x700 output, no borders, mwt floating at MWT_X,
# MWT_Y and a window with the app_id other below it; more lines after.
sway_conf() {
	{
		echo "output HEADLESS-1 resolution 1000x700 position 0 0"
		echo "default_border none"
		echo "default_floating_border none"
		echo "for_window [app_id=\"mwt\"] floating enable, move position $MWT_X $MWT_Y"
		echo "for_window [app_id=\"other\"] floating enable, move position 20 560"
		printf '%s\n' "$@"
	} >"$DIR/sway.conf"
}

# vinput, sway and the directory; a script with more to end does that first.
# Every job of the script too, before the wait: one left would hold it for good.
harness_cleanup() {
	[ -n "${VIN_PID:-}" ] && kill "$VIN_PID" 2>/dev/null
	[ -n "${SWAY:-}" ] && kill "$SWAY" 2>/dev/null
	local j
	for j in $(jobs -p); do kill "$j" 2>/dev/null; done
	wait 2>/dev/null
	rm -rf "$DIR"
}

# Both sockets, the Wayland one and sway's IPC, polled for, up to 5 s: swaymsg
# right after the first could find no IPC socket yet. Exports WAYLAND_DISPLAY and
# SWAYSOCK; false if they did not come. headless.sh, esctest.sh and
# profile-run.sh start sway themselves and wait with this.
sway_wait() {
	local i
	SWAYSOCK=
	for ((i = 0; i < 100; i++)); do
		SWAYSOCK=$(ls "$DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
		[ -S "$DIR/wayland-1" ] && [ -n "$SWAYSOCK" ] && [ -S "$SWAYSOCK" ] && break
		sleep 0.05
	done
	[ -S "$DIR/wayland-1" ] && [ -n "$SWAYSOCK" ] || return 1
	export WAYLAND_DISPLAY=wayland-1 SWAYSOCK
}

sway_start() {
	rm -f "$DIR/wayland-1" "$DIR"/sway-ipc.*
	sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
	SWAY=$!
	sway_wait || return 1
	# A sway started again (evil.sh compositor_dies) gets a vinput of its own.
	# The old one reaped first: bash, reaping a coproc, unsets VIN and VIN_PID,
	# and would take the new one's.
	if [ -n "${VIN_PID:-}" ]; then
		kill "$VIN_PID" 2>/dev/null
		wait "$VIN_PID" 2>/dev/null
	fi
	coproc VIN { "$VINPUT" 1000 700; }
	VIN_PID=${VIN_PID:-}
	[ -n "$VIN_PID" ] || { echo "vinput ended at once" >&2; return 1; }
	# Bash closes a coproc's descriptors in subshells, and `shown | grep` is one.
	exec 7>&"${VIN[1]}" 8<&"${VIN[0]}"
}
v() { echo "$*" >&7; read -r -t 3 _ <&8; }

# A new virtual keyboard per wtype call: sway hands its first key over in
# wl_keyboard.enter, which a client must not take as a press. So each call
# starts with a key mwt ignores.
T() { wtype -k Shift_L "$@"; sleep 0.3; }
K() { wtype -k Shift_L -M ctrl -M shift -k "$1" -m shift -m ctrl; sleep 0.3; }
C() { wtype -k Shift_L -M ctrl -k "$1" -m ctrl; sleep 0.3; }
# Shift held by a wtype in the background, while vinput clicks.
shift_click() { wtype -k Shift_L -M shift -s 800 -m shift & sleep 0.3; v click "${1:-left}"; wait $!; sleep 0.1; }

# window_rect [app_id]: "width height" of the window in sway's tree, or nothing;
# window_title [app_id] its title
window_rect() { swaymsg -t get_tree 2>/dev/null | python3 tests/swaytree.py size "${1:-mwt}"; }
window_title() { swaymsg -t get_tree 2>/dev/null | python3 tests/swaytree.py title "${1:-mwt}"; }

# cells <cols> <rows>: CW and CH, the cell in logical pixels, from the mwt
# window's size; px <col> <row> then X and Y, the middle of that cell.
cells() {
	local w h
	read -r w h < <(window_rect mwt)
	[ -n "${h:-}" ] || return 1
	CW=$(((w - 2 * PAD) / $1))
	CH=$(((h - 2 * PAD) / $2))
}
px() { X=$((MWT_X + PAD + $1 * CW + CW / 2)); Y=$((MWT_Y + PAD + $2 * CH + CH / 2)); }

# The whole screen into the primary selection, from its first cell past its
# last; the pause keeps two drags from counting as a double click. shown [file]
# prints it, one line per row, and keeps it in DIR/file (shown).
sel_screen() {
	sleep 0.45
	v move $((MWT_X + 1)) $((MWT_Y + 1))
	v down left
	v move 999 699
	v up left
	sleep 0.1
}
shown() {
	sel_screen
	printf '%s' "$(timeout 3 wl-paste -p -n 2>/dev/null)" >"$DIR/${1:-shown}"
	cat "$DIR/${1:-shown}"
}

# wait_window [app_id] [pid] [give_up]: until the window is in sway's tree, at
# most 10 s; with a pid, also until that process is gone, and with give_up until
# that command succeeds, for a window that never comes.
wait_window() {
	local i
	for ((i = 0; i < 100; i++)); do
		[ -n "$(window_rect "${1:-mwt}")" ] && return 0
		[ -n "${2:-}" ] && ! kill -0 "$2" 2>/dev/null && return 1
		[ -n "${3:-}" ] && "$3" && return 1
		sleep 0.1
	done
	return 1
}
