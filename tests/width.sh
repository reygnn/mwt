#!/bin/bash
# Where the cursor runs away: characters whose width mwt and a program count
# differently. Measures the widths (mwt by CPR, vim, tmux) and then looks for
# the damage in bash, vim and tmux under a headless sway, the screen read back
# through a mouse drag and the primary selection as in run.sh.
# Prints OK, GAP or INFO per check and fails on a GAP. The samples are in
# tests/widths.py. Needs sway, wtype, wl-clipboard, vinput, python3, vim, tmux.
set -u
cd "$(dirname "$0")/.." || exit 1
MWT=${MWT:-$PWD/mwt}
VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
W="python3 $PWD/tests/widths.py"
GAPS=0
for t in sway wtype wl-paste tic python3 vim tmux; do
	command -v "$t" >/dev/null || { echo "width: $t not installed"; exit 1; }
done
[ -x "$MWT" ] || { echo "width: $MWT not built"; exit 1; }
[ -x "$VINPUT" ] || { echo "width: $VINPUT not built"; exit 1; }

DIR=$(mktemp -d /tmp/mwt-width.XXXXXX) || exit 1
mkdir -p "$DIR/home" "$DIR/config"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/bash XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8 TMUX_TMPDIR=$DIR
unset WAYLAND_DISPLAY DISPLAY TERMINFO TERMINFO_DIRS TMUX
tic -x -o "$HOME/.terminfo" mwt.info || exit 1
cat >"$DIR/sway.conf" <<'EOF'
output HEADLESS-1 resolution 1000x700 position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 20 20
EOF

SWAY=
MPID=
cleanup() {
	[ -n "$MPID" ] && kill "$MPID" 2>/dev/null
	tmux -L width kill-server 2>/dev/null
	[ -n "${VIN_PID:-}" ] && kill "$VIN_PID" 2>/dev/null
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
[ -S "$DIR/wayland-1" ] || { echo "width: sway did not start"; exit 1; }
export WAYLAND_DISPLAY=wayland-1
sleep 0.3
coproc VIN { "$VINPUT" 1000 700; }
exec 7>&"${VIN[1]}" 8<&"${VIN[0]}"
v() { echo "$*" >&7; read -r -t 3 _ <&8; }

T() { wtype -k Shift_L "$@"; sleep 0.3; }
# The whole screen through the primary selection, one line per row.
shown() {
	sleep 0.45
	v move 21 21; v down left; v move 999 699; v up left
	sleep 0.1
	timeout 3 wl-paste -p -n 2>/dev/null >"$DIR/$1"
}
start_mwt() {
	"$MWT" "$@" >>"$DIR/log" 2>&1 &
	MPID=$!
	sleep 1.5
}
stop_mwt() {
	kill "$MPID" 2>/dev/null
	wait "$MPID" 2>/dev/null
	MPID=
}
report() {
	local line
	while IFS= read -r line; do
		echo "$line"
		[[ $line == GAP* ]] && GAPS=$((GAPS + 1))
	done
}

echo "=== widths: mwt (CPR), bash (readline, as mwt), vim, tmux"
start_mwt -e $W probe "$DIR/mwt.json"
for ((i = 0; i < 50; i++)); do
	[ -s "$DIR/mwt.json" ] && break
	sleep 0.1
done
stop_mwt
[ -s "$DIR/mwt.json" ] || { echo "GAP   the probe in mwt gave nothing"; GAPS=$((GAPS + 1)); echo '{}' >"$DIR/mwt.json"; }
$W vim "$DIR/vim.json"
$W tmux "$DIR/tmux.json"
report < <($W table "$DIR/mwt.json" "$DIR/vim.json" "$DIR/tmux.json")

echo "=== bash: a prompt with all samples, then an edit in the middle of the line"
printf 'PS1="%s \\$ "\nunset PROMPT_COMMAND\n' "$($W prompt)" >"$HOME/.bashrc"
start_mwt
T 'echo hallo' -k Left -k Left -k Left
T 'X'
shown bash
want="$($W prompt-visible) \$ echo haXllo"
if grep -qxF -- "$want" "$DIR/bash"; then
	echo "OK    bash: the line reads '$want'"
else
	echo "GAP   bash: no row '$want', the prompt row is: $(grep -F 'echo' "$DIR/bash" | head -1)"
	GAPS=$((GAPS + 1))
fi
stop_mwt

echo "=== vim: replace the last character of each sample row"
$W list "$DIR/samples.txt"
start_mwt -e vim -u NONE -N -i NONE -c 'set nowrap' "$DIR/samples.txt"
shown vim-before
T ':%normal $rX' -k Return
sleep 0.5
shown vim-after
report < <($W vim-check "$DIR/vim-before" "$DIR/vim-after")
stop_mwt

echo "=== tmux: each sample filled with dots to the edge of the left pane"
# The left pane waits for its text, which needs the pane width first.
start_mwt -e tmux -L width -f /dev/null new-session \
	"while [ ! -s '$DIR/padded.txt' ]; do sleep 0.1; done; cat '$DIR/padded.txt'; exec sleep 600" \; \
	split-window -h "exec sleep 600" \; select-pane -L
sleep 1
pw=$(tmux -L width display -p -t '{left}' '#{pane_width}' 2>/dev/null)
if [ -n "$pw" ]; then
	$W tmux-list "$DIR/padded.txt" "$pw" "$DIR/tmux.json"
	sleep 1
	shown tmux
	report < <($W tmux-check "$DIR/tmux" "$pw" "$DIR/tmux.json")
else
	echo "GAP   tmux did not start in mwt"
	GAPS=$((GAPS + 1))
fi
stop_mwt
tmux -L width kill-server 2>/dev/null

if grep -qE 'AddressSanitizer|runtime error' "$DIR/log" 2>/dev/null; then
	echo "CRASH sanitizer report from mwt: $(grep -m1 -E 'AddressSanitizer|runtime error' "$DIR/log")"
	GAPS=$((GAPS + 1))
fi
echo "=== $GAPS gaps"
[ "$GAPS" = 0 ]
