#!/bin/bash
# Container side of `make profile`, under a headless sway with Pixman. CPU time is
# mwt's alone, the compositor's does not count. The build keeps frame pointers and
# draw() apart from main(), so its times are a little above a plain -O2.
set -u
WORK=${PROFILE_WORK:-yes seq text sgr cjk screen cup videos cell scroll full latency}
SRC=$HOME/w/mwt
F=$HOME/w/profile
OUT=$F/out
mkdir -p "$SRC" "$OUT"
cp -r /src/mwt/. "$SRC"
cd "$SRC" || exit 1
make clean >/dev/null
CFLAGS="-O2 -g -fno-omit-frame-pointer -fno-optimize-sibling-calls -fno-inline-functions-called-once" \
	make -j"$(nproc)" 2>&1 | grep -E 'warning|error'
[ -x mwt ] || { echo "mwt did not build"; exit 1; }
gcc -O2 -shared -fPIC -fno-omit-frame-pointer tests/profsampler.c -o "$F/sampler.so" -ldl || exit 1
gcc -O2 -g -std=c11 -I. tests/profbench.c vt.c -o "$F/bench" -lutil -lm || exit 1
python3 tests/profgen.py "$F" tests/vt100/videos || exit 1

DIR=$(mktemp -d /tmp/prof.XXXXXX) || exit 1
mkdir -p "$DIR/home"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/sh XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8
unset DISPLAY WAYLAND_DISPLAY
tic -x -o "$HOME/.terminfo" mwt.info || exit 1
cat >"$DIR/sway.conf" <<CONF
output HEADLESS-1 resolution $PROFILE_RES position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 0 0
CONF
sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
SWAY=$!
trap 'kill "$SWAY" 2>/dev/null; rm -rf "$DIR"' EXIT
for ((i = 0; i < 50; i++)); do
	[ -S "$DIR/wayland-1" ] && break
	sleep 0.1
done
[ -S "$DIR/wayland-1" ] || { echo "sway did not start"; exit 1; }
export WAYLAND_DISPLAY=wayland-1
sleep 0.3

MWT="$SRC/mwt -g $PROFILE_GRID -e"
echo "grid $PROFILE_GRID, output $PROFILE_RES, $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"

# A run under the sampler: seconds, CPU, frames and where the time went.
profiled() {
	local name=$1
	shift
	PROF_OUT=$OUT/$name LD_PRELOAD=$F/sampler.so timeout 300 $MWT sh -c "$*"
	[ -s "$OUT/$name.stat" ] || { echo "  no samples, mwt did not end by itself"; return 1; }
	read -r wall user sys _ frames <"$OUT/$name.stat"
	awk -v w="$wall" -v u="$user" -v s="$sys" -v f="$frames" -v n="${2:-0}" 'BEGIN {
		c = u + s
		printf "  %.2f s, CPU %.2f s (%.0f %%), %d frames, %.1f/s, %.2f ms CPU per frame\n",
			w, c, 100 * c / w, f, f / w, 1000 * c / (f > 0 ? f : 1)
	}'
	python3 "$SRC/tests/profreport.py" "$OUT/$name" "$SRC/mwt"
}

flood() {
	local name=$1 file=$F/$1
	[ -f "$file" ] || { echo "=== $name: skipped, no tests/vt100 (tests/replay.sh --fetch on the host)"; return; }
	echo "=== flood $name: MB/s from memory $("$F/bench" mem "$file"), pty alone $("$F/bench" pty "$file"), pty + VT core $("$F/bench" pty+vt "$file")"
	profiled "$name" "cat '$file'"
}

paced() {
	local name=$1 report=$OUT/$1.ticks
	echo "=== 60 Hz $name"
	profiled "$name" "python3 '$SRC/tests/profpaced.py' $name '$report'"
	echo "  $(cat "$report") of 480 updates written in time"
}

# From Ctrl+C until the flood has stopped, sent by a real key press. The shell runs
# on after it, which is what shows the flood ended: yes dies, and the file appears.
# The wtype that only sleeps keeps a keyboard on the seat: without one no window
# has focus, and the key goes nowhere.
latency() {
	echo "=== Ctrl+C during yes, 5 times, ms:"
	local i j t0 t1 pid hold line=
	wtype -s 600000 &
	hold=$!
	sleep 0.3
	for i in 1 2 3 4 5; do
		rm -f "$DIR/done"
		$MWT sh -c "trap : INT; yes; : >'$DIR/done'" &
		pid=$!
		sleep 2
		t0=$(date +%s%N)
		wtype -k Shift_L -M ctrl -k c -m ctrl
		for ((j = 0; j < 2500; j++)); do
			[ -e "$DIR/done" ] && break
			sleep 0.002
		done
		t1=$(date +%s%N)
		if [ -e "$DIR/done" ]; then
			line="$line $(((t1 - t0) / 1000000))"
		else
			line="$line timeout"
			kill "$pid" 2>/dev/null
		fi
		wait "$pid"
	done
	kill "$hold" 2>/dev/null
	wait "$hold" 2>/dev/null
	echo " $line"
}

for w in $WORK; do
	case $w in
	cell | scroll | full) paced "$w" ;;
	latency) latency ;;
	*) flood "$w" ;;
	esac
done
