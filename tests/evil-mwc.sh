#!/bin/bash
# Cases that depend on the compositor, under a headless mwc. Results are read
# from files the shell writes, since mwc has no data-control for wl-paste.
# Fails on a GAP or a CRASH. Run with `make evil`; it needs a container.
set -u
[ -e /run/.containerenv ] || {
	echo "evil: only in a container, it kills every mwt, sleep and yes it finds: make evil"
	exit 1
}
cd "$(dirname "$0")/.." || exit 1
MWT=${MWT:-$PWD/mwt}
NAME=$(basename "$MWT")
MWC=${MWC:-${MWC_DIR:-../../mwc}/mwc}
VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
GAPS=0
CRASHES=0
for b in "$MWT" "$MWC" "$VINPUT"; do
	[ -x "$b" ] || { echo "evil: $b not built"; exit 1; }
done
DIR=$(mktemp -d /tmp/ev.XXXXXX) || exit 1
mkdir -p "$DIR/home" "$DIR/config"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/bash XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8 ASAN_OPTIONS=detect_leaks=0
tic -x -o "$HOME/.terminfo" mwt.info || exit 1
export MWC_TEST_STATE=$DIR/state MWC_TERMINAL=$MWT
unset WAYLAND_DISPLAY DISPLAY MWC_ACTIVE MWC_INACTIVE
printf 'PS1="\\w\\$ "\nunset PROMPT_COMMAND\n' >"$HOME/.bashrc"
CASE=none; LOG=$DIR/log.none; PID=
trap 'kill "${PID:-}" 2>/dev/null; pkill -x "$NAME" 2>/dev/null; rm -rf "$DIR"' EXIT
start_mwc() {
	rm -f "$DIR"/wayland-*
	"$MWC" -s 'exec sleep 3000' >"$DIR/mwc.log" 2>&1 &
	PID=$!
	for ((i = 0; i < 50; i++)); do [ -S "$DIR/wayland-0" ] && break; sleep 0.1; done
	export WAYLAND_DISPLAY=wayland-0
	sleep 0.3
	coproc VIN { "$VINPUT" 1280 720; }
	sleep 0.3
}
v() { echo "$*" >&"${VIN[1]}"; read -r -t 3 _ <&"${VIN[0]}"; }
start_mwc
T() { wtype -k Shift_L "$@"; sleep 0.3; }
K() { wtype -k Shift_L -M ctrl -M shift -k "$1" -m shift -m ctrl; sleep 0.3; }
C() { wtype -k Shift_L -M ctrl -k "$1" -m ctrl; sleep 0.3; }
# Selects by dragging from inside the view to its corner, which mwt takes as cell
# 0,0: x and y may be the frame's, and a drag that starts there moves the window.
sel_view() { local x y; x=$(get x); y=$(get y); v move $((x + 400)) $((y + 200)); v down left; v move "$x" "$y"; v up left; sleep 0.2; }
alive() { pgrep -x "$NAME" >/dev/null; }
count() { pgrep -xc "$NAME"; }
refresh() { rm -f "$DIR/state"; kill -USR1 "$PID"; for ((i = 0; i < 100; i++)); do [ -e "$DIR/state" ] && return; sleep 0.02; done; }
get() { refresh; awk -v k="$1=" '$1 == "view" && $2 == "app=mwt" { for (i = 3; i <= NF; i++) if (index($i, k) == 1) { print substr($i, length(k) + 1); exit } }' "$DIR/state"; }
start_mwt() { LOG=$DIR/log.$CASE; (cd "$HOME" && sh -c '"$0" >>"$1" 2>&1 &' "$MWT" "$LOG"); sleep 1.5; }
stop_mwt() { pkill -x "$NAME" 2>/dev/null; sleep 0.3; pkill -9 -x "$NAME" 2>/dev/null; pkill -x yes 2>/dev/null; pkill -x seq 2>/dev/null; pkill -f 'sleep 7[0-9][0-9]' 2>/dev/null; sleep 0.3; }
ok() { echo "OK    [$CASE] $*"; }
gap() { GAPS=$((GAPS + 1)); echo "GAP   [$CASE] $*"; }
info() { echo "INFO  [$CASE] $*"; }
waitfile() { local i; for ((i = 0; i < $2 * 5; i++)); do [ -s "$1" ] && return 0; sleep 0.2; done; return 1; }
run() {
	CASE=$1
	if [ -n "${ONLY:-}" ] && ! [[ " $ONLY " == *" $1 "* ]]; then return; fi
	local t0=$SECONDS
	"$1"
	if grep -q 'AddressSanitizer\|runtime error' "$LOG" 2>/dev/null; then CRASHES=$((CRASHES + 1)); echo "CRASH [$CASE]"; grep -m1 -A14 'AddressSanitizer\|runtime error' "$LOG"; fi
	if grep -qi 'critical\|protocol error\|lost the connection' "$LOG" 2>/dev/null; then echo "LOG   [$CASE] $(grep -i -m3 'critical\|protocol error\|lost the connection' "$LOG" | cut -c1-160 | tr '\n' '|')"; fi
	stop_mwt
	rm -f "$HOME"/res*
	echo "      ($1: $((SECONDS - t0))s)"
}
basic() {
	start_mwt
	T 'echo ja >~/res' -k Return
	if waitfile "$HOME/res" 4; then ok "mwt runs under mwc, wtype reaches it"; else gap "no input reaches mwt under mwc"; fi
}
states() {
	start_mwt
	T 'stty size >~/res0' -k Return; sleep 0.5
	local n=0
	v key alt+F10; sleep 0.6
	T 'stty size >~/res1' -k Return; sleep 0.5
	v key alt+F5; sleep 0.6
	T 'stty size >~/res2' -k Return; sleep 0.5
	v key alt+F11; sleep 0.6
	T 'stty size >~/res3' -k Return; sleep 0.5
	v key alt+F11; sleep 0.6
	T 'stty size >~/res4' -k Return; sleep 0.5
	info "stty size normal/max/restored/full/back: $(cat "$HOME"/res0 "$HOME"/res1 "$HOME"/res2 "$HOME"/res3 "$HOME"/res4 | tr '\n' '/')"
	if [ "$(cat "$HOME/res0")" = "$(cat "$HOME/res2")" ] && [ "$(cat "$HOME/res0")" != "$(cat "$HOME/res1")" ] && [ "$(cat "$HOME/res0")" = "$(cat "$HOME/res4")" ]; then ok "maximize, restore, fullscreen, back all reach the pty"; else gap "window state and pty size disagree"; fi
}
minimized_flood() {
	start_mwt
	T 'sleep 2; seq 1 400000; echo fertig >~/res' -k Return
	v key alt+F9; sleep 0.3
	if waitfile "$HOME/res" 25; then ok "minimized (no frame callbacks): output still drains"; else gap "minimized mwt stops reading output"; fi
	alive || gap "mwt died while minimized"
}
flood_states() {
	start_mwt
	T 'yes "äöü 漢字 $(printf x%.0s {1..150})"' -k Return
	local i
	for i in 1 2 3 4 5 6 7 8; do v key alt+F10; sleep 0.15; v key alt+F5; sleep 0.15; v key alt+F11; sleep 0.15; v key alt+F11; sleep 0.15; v key alt+F9; sleep 0.15; v key alt+Tab; sleep 0.15; done
	alive || { gap "mwt died during window state changes in a flood"; return; }
	C c; sleep 1
	T 'echo ja >~/res' -k Return
	if waitfile "$HOME/res" 5; then ok "window state changes through a flood"; else info "mwt alive, input did not arrive (focus left it?)"; fi
}
close_keys() {
	start_mwt
	T 'sleep 777' -k Return; sleep 0.4
	v key alt+F4; sleep 1.5
	if alive; then gap "Alt+F4 does not close mwt"; else ok "Alt+F4 ends mwt"; fi
	if pgrep -f 'sleep 777' >/dev/null; then gap "the program in the window survives Alt+F4"; else ok "the program in the window gets its hangup"; fi
}
copy_between() {
	start_mwt; sleep 0.2
	T 'echo kopier-mich' -k Return; sleep 0.3
	sel_view; K c
	start_mwt
	T 'cat >~/res' -k Return; sleep 0.3
	K v; sleep 0.5
	C d; sleep 0.5
	if grep -q 'kopier-mich' "$HOME/res" 2>/dev/null; then ok "Ctrl+Shift+C in one mwt, Ctrl+Shift+V in another"; else gap "clipboard between two mwt windows: got '$(head -c 80 "$HOME/res" 2>/dev/null)'"; fi
}
primary_between() {
	start_mwt
	T 'echo prim-text' -k Return; sleep 0.3
	sel_view
	start_mwt
	T 'cat >~/res' -k Return; sleep 0.3
	local x y
	x=$(get x); y=$(get y)
	v move $((x + 200)) $((y + 100)); v click middle; sleep 0.6
	C d; sleep 0.5
	if grep -q 'prim-text' "$HOME/res" 2>/dev/null; then ok "primary selection between two mwt windows"; else gap "primary between two mwt windows: got '$(head -c 80 "$HOME/res" 2>/dev/null)'"; fi
}
mwc_spawns() {
	start_mwt
	v key ctrl+alt+Return; sleep 1.5
	if [ "$(count)" = 2 ]; then ok "Ctrl+Alt+Return starts mwt from mwc"; else gap "mwc's terminal key: $(count) mwt"; fi
	T 'exit' -k Return; sleep 1.5
	[ "$(count)" = 1 ] && ok "that window closes on exit" || gap "window did not close on exit: $(count)"
}
# mwc has to hand on the notches: six of them, six reports.
wheel_notches() {
	start_mwt
	T "printf '\\033[?1000;1006h'; cat -v >~/res" -k Return; sleep 0.3
	local x y i n
	x=$(get x); y=$(get y)
	v move $((x + 200)) $((y + 100))
	for i in 1 2 3 4 5 6; do v wheel 1; sleep 0.1; done
	sleep 0.3
	T -k Return; sleep 0.2
	C d; sleep 0.5
	n=$(grep -o '\[<65;' "$HOME/res" 2>/dev/null | wc -l)
	if [ "$n" = 6 ]; then ok "six wheel notches under mwc are six reports"; else gap "six wheel notches under mwc gave $n reports"; fi
}
tiling() {
	start_mwt; start_mwt; start_mwt
	v key super+t 2>/dev/null; sleep 0.8
	T 'stty size >~/res' -k Return; sleep 0.5
	info "with 3 windows, tiling key: size $(cat "$HOME/res" 2>/dev/null)"
	[ "$(count)" = 3 ] && ok "3 windows survive" || gap "only $(count) of 3 left"
}
compositor_dies() {
	start_mwt
	T 'sleep 778' -k Return; sleep 0.4
	kill -9 "$PID"; sleep 2
	if alive; then gap "mwt survives mwc"; else ok "mwt ends with mwc"; fi
	if pgrep -f 'sleep 778' >/dev/null; then gap "program stays after mwc died"; else ok "program ended with it"; fi
	start_mwc
}
for c in basic states minimized_flood flood_states close_keys copy_between primary_between mwc_spawns wheel_notches tiling compositor_dies; do run "$c"; done
echo "=== $GAPS gaps, $CRASHES crashes ==="
[ "$GAPS" = 0 ] && [ "$CRASHES" = 0 ]
