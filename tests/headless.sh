#!/bin/bash
# Runs a Python test under a headless sway of its own, so a running session is not
# touched: tests/headless.sh tests/pixel.py. tests/mwtlib.py is its side of this.
# Needs sway, wtype, grim, tic, python3 and vinput from mwc.
set -u
cd "$(dirname "$0")/.." || exit 1
export MWT=${MWT:-$PWD/mwt}
export VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
export PYTHONDONTWRITEBYTECODE=1
for t in sway wtype grim tic python3; do
	command -v "$t" >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
[ -x "$MWT" ] || { echo "FAIL: $MWT not built"; exit 1; }
[ -x "$VINPUT" ] || { echo "FAIL: $VINPUT not built (make build/vinput in mwc)"; exit 1; }

DIR=$(mktemp -d /tmp/mwt-headless.XXXXXX) || exit 1
export DIR
mkdir -p "$DIR/home"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/sh XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8
unset WAYLAND_DISPLAY DISPLAY TERMINFO TERMINFO_DIRS
tic -x -o "$HOME/.terminfo" mwt.info || { echo "FAIL: mwt.info does not compile"; exit 1; }
cat >"$DIR/sway.conf" <<'CONF'
output HEADLESS-1 resolution 1000x700 position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 20 20
for_window [app_id="other"] floating enable, move position 20 560
CONF

SWAY=
cleanup() {
	[ -n "$SWAY" ] && kill "$SWAY" 2>/dev/null
	wait 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup EXIT
sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
SWAY=$!
for ((i = 0; i < 50; i++)); do
	[ -S "$DIR/wayland-1" ] && break
	sleep 0.1
done
[ -S "$DIR/wayland-1" ] || { echo "FAIL: sway did not start"; exit 1; }
export WAYLAND_DISPLAY=wayland-1
sleep 0.3
SWAYSOCK=$(ls "$DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
export SWAYSOCK
PYTHONPATH=$PWD/tests python3 "$@"
