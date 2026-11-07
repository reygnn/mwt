#!/bin/bash
# esctest, George Nachman's and Thomas Dickey's suite of escape sequence tests, run
# inside mwt under a headless sway. It reads the screen back with DECRQCRA, which
# mwt answers only when allowed, so it runs mwt-esctest (make mwt-esctest). esctest
# is not in this repository, it is GPL; the first run clones it, pinned to one
# commit, into tests/esctest (ESC_DIR). Without network, run
# `tests/esctest.sh --fetch` once beforehand.
#
# A report, it fails only when esctest could not run: mwt leaves out much of what
# esctest covers (window operations, left and right margins, rectangle and selective
# erase operations, color queries), and those tests fail by design.
#
#   tests/esctest.sh                  run all tests, print the summary and failures
#   tests/esctest.sh --include=REGEX  only some, other options go to esctest too
#   tests/esctest.sh --fetch          only fetch esctest
set -u
cd "$(dirname "$0")/.." || exit 1
ESC_DIR=${ESC_DIR:-$PWD/tests/esctest}
MWT=${MWT:-$PWD/mwt-esctest}
REPO=https://github.com/ThomasDickey/esctest2
COMMIT=2798f12149a19c3295e9b4853ab2da4b2eff1b2b

fetch() {
	[ -f "$ESC_DIR/esctest/esctest.py" ] && return 0
	echo "esctest: fetching $REPO at ${COMMIT:0:12} into $ESC_DIR"
	rm -rf "$ESC_DIR.tmp"
	git init -q "$ESC_DIR.tmp" &&
	git -C "$ESC_DIR.tmp" fetch -q --depth 1 "$REPO" "$COMMIT" &&
	git -C "$ESC_DIR.tmp" -c advice.detachedHead=false checkout -q FETCH_HEAD &&
	mv "$ESC_DIR.tmp" "$ESC_DIR" || { echo "esctest: fetch failed"; rm -rf "$ESC_DIR.tmp"; return 1; }
}

if [ "${1:-}" = --fetch ]; then
	fetch
	exit
fi
fetch || exit 1
for t in sway tic python3; do
	command -v "$t" >/dev/null || { echo "esctest: $t not installed"; exit 1; }
done
[ -x "$MWT" ] || { echo "esctest: $MWT not built (make mwt-esctest)"; exit 1; }

DIR=$(mktemp -d /tmp/mwt-esctest.XXXXXX) || exit 1
mkdir -p "$DIR/home" "$DIR/config"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/sh XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8
unset WAYLAND_DISPLAY DISPLAY TERMINFO TERMINFO_DIRS
tic -x -o "$HOME/.terminfo" mwt.info || exit 1
cat >"$DIR/sway.conf" <<'CONF'
output HEADLESS-1 resolution 1000x700 position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 0 0
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
[ -S "$DIR/wayland-1" ] || { echo "esctest: sway did not start"; exit 1; }
export WAYLAND_DISPLAY=wayland-1
sleep 0.3

# As xterm at VT420: esctest reads the screen back only from VT level 4 on. mwt
# answers DA and DA2 as a VT220, so those tests fail. A report that does not
# come fails a test after 0.3 s instead of the default second.
LOG=$DIR/esctest.log
timeout 1800 "$MWT" -g 80x24 -e sh -c 'log=$1; shift; cd "$0/esctest" && exec python3 esctest.py \
	--expected-terminal=xterm --max-vt-level=4 --timeout=0.3 --no-print-logs \
	--logfile="$log" "$@" 2>"$log.err"' "$ESC_DIR" "$LOG" "$@" >"$DIR/mwt.log" 2>&1
if ! grep -q '^\*\*\* .* passed' "$LOG" 2>/dev/null; then
	echo "esctest: did not finish"
	tail -n 20 "$DIR/mwt.log" "$LOG.err" "$LOG" 2>/dev/null
	exit 1
fi
python3 - "$LOG" <<'PY'
import collections, re, sys
log = open(sys.argv[1], errors="replace").read()
by_class = collections.Counter()
timeouts = 0
for block in re.split(r"\nRun test: ", log)[1:]:
    if "FAILED:" in block:
        by_class[block.split(".", 1)[0]] += 1
        timeouts += "Timeout waiting to read" in block
print("esctest: failures by group, " + ", ".join(f"{k} {v}" for k, v in sorted(by_class.items())))
print(f"esctest: {timeouts} of them wait for a report mwt does not send")
print(re.findall(r"^\*\*\* .*\*\*\*$", log, re.M)[-1])
PY
