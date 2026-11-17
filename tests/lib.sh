# tests/lib.sh - what run.sh, evil.sh and width.sh share: a headless sway of
# their own, vinput from mwc as the mouse, wtype as the keyboard, the window's
# size from sway's tree. Sourced, with DIR and VINPUT set and DIR/sway.conf
# written; sway_start sets SWAY, VIN_PID, WAYLAND_DISPLAY and SWAYSOCK.

# Both sockets, the Wayland one and sway's IPC, polled for: swaymsg right after
# the first could find no IPC socket yet.
sway_start() {
	local i
	rm -f "$DIR/wayland-1" "$DIR"/sway-ipc.*
	sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
	SWAY=$!
	SWAYSOCK=
	for ((i = 0; i < 100; i++)); do
		SWAYSOCK=$(ls "$DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
		[ -S "$DIR/wayland-1" ] && [ -n "$SWAYSOCK" ] && [ -S "$SWAYSOCK" ] && break
		sleep 0.05
	done
	[ -S "$DIR/wayland-1" ] && [ -n "$SWAYSOCK" ] || return 1
	export WAYLAND_DISPLAY=wayland-1 SWAYSOCK
	coproc VIN { "$VINPUT" 1000 700; }
	VIN_PID=$VIN_PID
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

# window_rect [app_id]: "width height" of the window in sway's tree, or nothing
window_rect() {
	swaymsg -t get_tree 2>/dev/null | python3 -c '
import json, sys
def walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from walk(c)
try:
    tree = json.load(sys.stdin)
except ValueError:
    sys.exit()
for n in walk(tree):
    if n.get("app_id") == sys.argv[1]:
        print(n["rect"]["width"], n["rect"]["height"])
        break' "${1:-mwt}"
}

# wait_window [app_id] [pid]: until the window is in sway's tree, at most 10 s;
# with a pid, also until that process is gone, for a window that never comes.
wait_window() {
	local i
	for ((i = 0; i < 100; i++)); do
		[ -n "$(window_rect "${1:-mwt}")" ] && return 0
		[ -n "${2:-}" ] && ! kill -0 "$2" 2>/dev/null && return 1
		sleep 0.1
	done
	return 1
}
