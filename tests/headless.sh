#!/bin/bash
# Runs a Python test under a headless sway of its own, so a running session is not
# touched: tests/headless.sh tests/pixel.py. tests/mwtlib.py is its side of this.
# Needs sway, wtype, grim, tic, python3 and vinput from mwc.
set -u
cd "$(dirname "$0")/.." || exit 1
. tests/lib.sh
export MWT=${MWT:-$PWD/mwt}
export VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
# tests/links.py checks the token with it where it is built (make build/xdgc in mwc)
[ -x "${XDGC:-${MWC_DIR:-../../mwc}/build/xdgc}" ] && export XDGC=${XDGC:-$(readlink -f "${MWC_DIR:-../../mwc}/build/xdgc")}
export PYTHONDONTWRITEBYTECODE=1
for t in sway wtype grim tic python3; do
	command -v "$t" >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
[ -x "$MWT" ] || { echo "FAIL: $MWT not built"; exit 1; }
[ -x "$VINPUT" ] || { echo "FAIL: $VINPUT not built (make build/vinput in mwc)"; exit 1; }

HARNESS_SHELL=/bin/sh harness_dir headless
# sway 1.9 draws the pointer into what grim captures, over the cells under test
sway_conf "seat * hide_cursor 100"
SWAY=
trap harness_cleanup EXIT
# SWAY_WAYLAND_DEBUG: sway logs the protocol too, as tests/wayland.py reads it
env ${SWAY_WAYLAND_DEBUG:+WAYLAND_DEBUG=server} sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
SWAY=$!
sway_wait || { echo "FAIL: sway did not start"; exit 1; }
PYTHONPATH=$PWD/tests python3 "$@"
