#!/bin/bash
# Adversarial black-box cases for mwt under a headless sway: hostile output,
# pastes and terminal settings, broken arguments, abuse of mouse and window.
# What mwt shows is read through a mouse drag and the primary selection.
# Prints OK, GAP, INFO or CRASH per case and fails on a GAP or a CRASH.
# Run with `make evil`; it needs a container. ONLY="case case" picks cases,
# VIN_LAYOUTS=1 says that $VINPUT takes its layout from VIN_LAYOUT.
set -u
[ -e /run/.containerenv ] || {
	echo "evil: only in a container, it kills every mwt, sleep and yes it finds: make evil"
	exit 1
}
cd "$(dirname "$0")/.." || exit 1
MWT=${MWT:-$PWD/mwt}
NAME=$(basename "$MWT")
VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
ONLY=${ONLY:-}
GAPS=0
CRASHES=0
[ -x "$MWT" ] || { echo "evil: $MWT not built"; exit 1; }
[ -x "$VINPUT" ] || { echo "evil: $VINPUT not built"; exit 1; }

DIR=$(mktemp -d /tmp/ev.XXXXXX) || exit 1
mkdir -p "$DIR/home" "$DIR/config"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/bash XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8
export ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1
unset WAYLAND_DISPLAY DISPLAY MWC_ACTIVE MWC_INACTIVE
printf 'PS1="\\w\\$ "\nunset PROMPT_COMMAND\n' >"$HOME/.bashrc"
# A clipboard reader that asks and never reads, for stalled_pipes.
STALL=$DIR/stallreader
SCAN=$(pkg-config --variable=wayland_scanner wayland-scanner)
XML=tests/wlr-data-control-unstable-v1.xml
{ "$SCAN" client-header "$XML" "$DIR/wlr-data-control-unstable-v1-client-protocol.h" &&
	"$SCAN" private-code "$XML" "$DIR/data-control.c" &&
	cc -std=c11 -O1 -I"$DIR" -o "$STALL" tests/stallreader.c "$DIR/data-control.c" \
		$(pkg-config --cflags --libs wayland-client); } >"$DIR/stall.build" 2>&1 || STALL=
tic -x -o "$HOME/.terminfo" mwt.info || exit 1
cat >"$DIR/sway.conf" <<'EOF'
output HEADLESS-1 resolution 1000x700 position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 20 20
EOF

trap 'kill "${SWAY:-}" 2>/dev/null; pkill -x "$NAME" 2>/dev/null; rm -rf "$DIR"' EXIT
SWAY=
LOG=$DIR/log.none
CASE=none
start_sway() {
	rm -f "$DIR/wayland-1" "$DIR"/sway-ipc.*
	sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
	SWAY=$!
	for ((i = 0; i < 50; i++)); do
		[ -S "$DIR/wayland-1" ] && break
		sleep 0.1
	done
	export WAYLAND_DISPLAY=wayland-1
	sleep 0.3
	export SWAYSOCK=$(ls "$DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
	coproc VIN { "$VINPUT" 1000 700; }
	# Bash closes a coproc's descriptors in subshells, and `shown | grep` is one.
	exec 7>&"${VIN[1]}" 8<&"${VIN[0]}"
}
v() { echo "$*" >&7; read -r -t 3 _ <&8; }
start_sway

T() { wtype -k Shift_L "$@"; sleep 0.3; }
# Shift held by a wtype in the background, while vinput clicks.
shift_click() { wtype -k Shift_L -M shift -s 800 -m shift & sleep 0.3; v click "${1:-left}"; wait $!; sleep 0.1; }
K() { wtype -k Shift_L -M ctrl -M shift -k "$1" -m shift -m ctrl; sleep 0.3; }
C() { wtype -k Shift_L -M ctrl -k "$1" -m ctrl; sleep 0.3; }
# Selects the whole screen, or with sel_history everything from the top of the
# scrollback. The pause keeps two drags from counting as a double click.
sel_screen() { sleep 0.45; v move 21 21; v down left; v move 999 699; v up left; sleep 0.1; }
pages() { local a=() i; for ((i = 0; i < $2; i++)); do a+=(-k "$1"); done; wtype -k Shift_L -M shift "${a[@]}" -m shift; sleep 0.5; }
sel_history() { pages Prior 230; sleep 0.45; v move 21 21; v down left; v move 60 21; v up left; pages Next 230; v move 500 $R_LAST; shift_click; sleep 0.2; }
shown() { sel_screen; printf '%s' "$(timeout 3 wl-paste -p -n 2>/dev/null)" >"$DIR/shown"; cat "$DIR/shown"; }
try() { local n=$1 p=$2 i; for ((i = 0; i < n; i++)); do shown | grep -qx -- "$p" && return 0; sleep 0.2; done; return 1; }
alive() { pgrep -x "$NAME" >/dev/null; }
mwtpid() { pgrep -xn "$NAME"; }

# The middle of rows 1 and 2 and of the last row, in pixels, for the 80x24 window.
measure() {
	local w h
	read -r w h < <(swaymsg -t get_tree | python3 -c 'import json,sys
def walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from walk(c)
for n in walk(json.load(sys.stdin)):
    if n.get("app_id") == "mwt":
        print(n["rect"]["width"], n["rect"]["height"])
        break')
	[ -n "${h:-}" ] || return
	CH=$(((h - 4) / 24))
	R1=$((22 + CH + CH / 2))
	R2=$((R1 + CH))
	R_LAST=$((22 + 23 * CH + CH / 2))
}
CH=
R1=57
R2=76
R_LAST=450
start_mwt() {
	LOG=$DIR/log.$CASE
	(cd "$HOME" && env "$@" sh -c '"$0" >"$1" 2>&1 &' "$MWT" "$LOG")
	sleep 1.5
	[ -n "${CH:-}" ] || measure
}
stop_mwt() {
	pkill -x "$NAME" 2>/dev/null
	sleep 0.3
	pkill -9 -x "$NAME" 2>/dev/null
	pkill -x sleep 2>/dev/null
	pkill -x yes 2>/dev/null
	sleep 0.2
}
ok() { echo "OK    [$CASE] $*"; }
gap() { GAPS=$((GAPS + 1)); echo "GAP   [$CASE] $*"; }
info() { echo "INFO  [$CASE] $*"; }
check_log() {
	if grep -q 'AddressSanitizer\|runtime error\|LeakSanitizer' "$LOG" 2>/dev/null; then
		CRASHES=$((CRASHES + 1))
		echo "CRASH [$CASE] sanitizer:"
		grep -m1 -A14 'AddressSanitizer\|runtime error' "$LOG"
	fi
	if grep -qi 'critical\|warning\|protocol error' "$LOG" 2>/dev/null; then
		echo "LOG   [$CASE] $(grep -i -m3 'critical\|warning\|protocol error' "$LOG" | cut -c1-200 | tr '\n' '|')"
	fi
}
# run <name> <function>
run() {
	CASE=$1
	if [ -n "$ONLY" ] && ! [[ " $ONLY " == *" $1 "* ]]; then return; fi
	local t0=$SECONDS
	"$1"
	check_log
	stop_mwt
	echo "      ($1: $((SECONDS - t0))s)"
}

# ---- generators
cat >"$HOME/fuzz.py" <<'EOF'
import os, random, sys
r = random.Random(int(sys.argv[1]))
total = int(sys.argv[2])
frags = [b"\x1b", b"\x1b[", b"\x1b]", b"\x1bP", b"\x07", b"\x1b\\", b"\x18", b"\x1a", b"\r", b"\n", b"\b", b"\t", b"\x0b", b"\x0c", b"\x7f", b"\0",
  b"\x1b[K", b"\x1b[1K", b"\x1b[2K", b"\x1b[99999C", b"\x1b[99999D", b"\x1b[0G", b"\x1b[40G", b"\x1b[;;;K", b"\x1b[?25h", b"\x1b[-5C",
  b"\xc3", b"\xe2\x82", b"\xf0\x9f\x98", b"\xff", b"\xc0\x80", b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\xef\xbf\xbe", b"\xef\xbf\xbf", b"\xf4\x8f\xbf\xbf",
  "ä".encode(), "€".encode(), "漢字".encode(), "🙂".encode(), "👩‍👩‍👧‍👦".encode(), "é̂̃̄̅̆".encode(), "⃐".encode(), "‍".encode(), "️".encode(), "̀".encode(), "‮".encode(), " ".encode(), " ".encode(), "﻿".encode(), "\U000e0041".encode(), "נסיון".encode(), "مرحبا".encode(), "नि".encode(), "\xad".encode(), "<b>&amp;".encode(),
  b"x" * 79, b"y" * 80, b"z" * 81, "漢".encode() * 41]
out = bytearray()
while len(out) < total:
    if r.random() < 0.05:
        out += bytes(r.randrange(256) for _ in range(r.randrange(40)))
    else:
        out += r.choice(frags)
fd = 1
out += b"\x18\r\n"
view = memoryview(out)
while view:
    n = os.write(fd, view[:r.randrange(1, 9000)])
    view = view[n:]
EOF
cat >"$HOME/flip.sh" <<'EOF'
#!/bin/bash
# the current row flips between one and two byte characters
a=$(printf 'a%.0s' {1..70}); b=$(printf 'ä%.0s' {1..70})
while :; do printf '%s\r' "$a"; sleep 0.013; printf '%s\r' "$b"; sleep 0.017; done
EOF
python3 - "$DIR" <<'EOF'
import random, sys
d = sys.argv[1]
r = random.Random(7)
alpha = "abcdefgh äöüßéèñ€漢字🙂нет"
with open(d + "/integrity.txt", "w", encoding="utf-8") as f:
    size = 0
    while size < 1000000:
        n = r.choice([0, 1, 5, 80, 81, 200, 1000, 3000])
        s = "".join(r.choice(alpha) for _ in range(n))
        s = s.encode()[:3990].decode(errors="ignore")
        f.write(s + "\n")
        size += len(s.encode()) + 1
with open(d + "/big.txt", "w") as f:
    for i in range(17 * 1024 * 1024 // 64 + 1000):
        f.write(("%063d" % i) + "\n")
with open(d + "/colon.txt", "w") as f:
    for i in range(20000):
        f.write(": %060d\n" % i)
with open(d + "/slow.txt", "w") as f:
    for i in range(3000):
        f.write("sleep 0.01; echo t >>~/ticks\n")
EOF

# Comments, a long line and a pipeline over lines with backslashes.
cat >"$DIR/snippet.sh" <<'SNIPPET'
# 1) cat: Inhalt einer Datei ausgeben, hier die Infos zu deiner Linux-Distribution
cat /etc/os-release

# 2) grep: alle Benutzer mit Bash als Login-Shell finden, -n zeigt die Zeilennummern
grep -n "/bin/bash" /etc/passwd  # Kommentar am Zeilenende

# 3) Pipeline über mehrere Zeilen (mit \): die 5 größten Dateien in /etc nach Größe sortiert anzeigen. Dieser Kommentar ist absichtlich sehr lang, damit du sehen kannst, ob dein Terminal lange Zeilen sauber umbricht oder abschneidet
ls -la /etc \
  | sort -k5 -n -r \
  | head -n 5 \
  | awk '{print $5, $9}'
SNIPPET
bash "$DIR/snippet.sh" >"$DIR/snippet.out" 2>&1

# ---- output parser
dcs_swallow() {
	start_mwt
	T 'printf "\033P"' -k Return
	sleep 0.5
	T 'echo sichtbar-1' -k Return
	if try 8 'sichtbar-1'; then ok "output comes back after an unterminated DCS"; return; fi
	gap "ESC P with no terminator: every later line of output disappears, prompt included"
}
osc_swallow_cat() {
	start_mwt
	printf 'text before\033]0;never ends\nline2\nline3\n' >"$HOME/bin.dat"
	T 'cat ~/bin.dat' -k Return
	T 'echo sichtbar-3' -k Return
	if try 8 'sichtbar-3'; then ok "cat of a file with a stray ESC ]"; else gap "cat of a file containing one stray ESC ] blanks the terminal for good"; fi
}
fuzz() {
	start_mwt
	local s
	for s in 1 2 3 4; do
		T "python3 ~/fuzz.py $s 1500000; echo fuzz-$s-ende" -k Return
		local i
		for i in 1 2 3 4 5 6; do
			swaymsg -q "[app_id=\"mwt\"] resize set $((200 + RANDOM % 700)) $((100 + RANDOM % 500)), move position 20 20" 2>/dev/null
			sel_screen; K c
			sleep 0.4
			alive || break 2
		done
		sleep 2
		alive || break
	done
	if alive; then
		# the terminal's answers to queries in the noise wait in the input line
		C u
		T 'echo fuzz-alive' -k Return
		if try 12 'fuzz-alive'; then ok "4 x 1.5 MB fuzz with resizes"; else gap "alive after fuzz, but output does not come back: $(tail -3 "$DIR/shown" | cut -c1-70 | tr '\n' '|')"; fi
	else
		gap "mwt died in the fuzz"
		tail -30 "$LOG"
	fi
}
noncharacters() {
	start_mwt
	T 'python3 -c "import sys;sys.stdout.buffer.write(b\"[\\xef\\xbf\\xbf|\\xef\\xbf\\xbe|\\xf4\\x8f\\xbf\\xbf|\\xf3\\xa0\\x80\\x81]\\n\")"' -k Return
	sleep 0.5
	shown >/dev/null
	if python3 -c "open('$DIR/shown','rb').read().decode()" 2>/dev/null; then ok "noncharacters survive drawing and copying"; else gap "copy is not valid UTF-8 after noncharacters"; fi
}

# ---- selection
mid_char_selection() {
	start_mwt
	C l
	T 'bash ~/flip.sh' -k Return
	sleep 0.5
	local bad=0 n
	for ((n = 1; n <= 80; n++)); do
		v move $((90 + n % 12 * 9)) $R1
		v down left
		sleep 0.0$((RANDOM % 9 + 1))
		v move $((200 + n % 12 * 8)) $R1
		sleep 0.0$((RANDOM % 9 + 1))
		v up left
		sleep 0.15
		timeout 2 wl-paste -p -n >"$DIR/prim" 2>/dev/null
		python3 -c "open('$DIR/prim','rb').read().decode()" 2>/dev/null || { bad=$((bad + 1)); cp "$DIR/prim" "$DIR/prim.bad"; }
		alive || break
	done
	C c
	if ! alive; then gap "mwt died while a selection crossed a changing row"; return; fi
	if [ "$bad" -gt 0 ]; then gap "$bad/80 selections hand out invalid UTF-8 (cut inside a character): $(od -An -c "$DIR/prim.bad" | head -2 | tr -s ' ' | cut -c1-80)"; else ok "80 selections over a changing row, none cut inside a character"; fi
}
drag_during_flood() {
	start_mwt
	T 'seq 1 2000000' -k Return
	v move 100 100; v down left
	local i
	for i in 1 2 3 4 5 6 7 8; do v move $((100 + i * 20)) $((100 + i * 60)); sleep 0.2; done
	v move 100 690; sleep 1.5
	C l 2>/dev/null
	v up left
	C c
	sleep 1
	T 'echo drag-ende' -k Return
	if alive && try 12 'drag-ende'; then ok "drag past the edge through a flood and trims, Ctrl+L mid drag"; else gap "drag during a flood broke mwt"; fi
}
select_history() {
	start_mwt
	T 'seq 1 30000' -k Return
	sleep 3
	pages Prior 230
	shown >/dev/null
	local top
	top=$(grep -xE '[0-9]+' "$DIR/shown" | head -1)
	if [ "${top:-0}" -ge 24970 ] && [ "${top:-0}" -le 24990 ]; then ok "scrollback keeps its 5000 lines, the top shows $top"; else gap "top of the scrollback after 30000 lines: '$top'"; fi
	pages Next 230
	sel_history
	K c
	local n last
	n=$(timeout 8 wl-paste -n | wc -l)
	last=$(timeout 8 wl-paste -n | grep -xE '[0-9]+' | tail -1)
	if [ "$last" = 30000 ] && [ "$n" -ge 5000 ] && [ "$n" -le 5030 ]; then ok "a selection over the whole scrollback, $n lines"; else gap "selection over the scrollback: $n lines, last '$last'"; fi
}
click_clusters() {
	start_mwt
	C l
	T 'python3 -c "print(\"漢字 👩‍👩‍👧‍👦 éé נסיון abc مرحبا ﻿   x\"*3)"' -k Return
	sleep 0.5
	local x
	for x in 40 90 140 200 260 320 380 440 500 560 620; do
		v move "$x" $R1; v click left; v click left; sleep 0.05
		v click left; sleep 0.45
		v move "$x" $R2; v down left; v move $((x + 77)) $R1; v up left; sleep 0.1
		alive || break
	done
	if alive; then ok "clicks and drags over clusters, RTL text, separators"; else gap "mwt died on clicks over clusters"; fi
}

# ---- paste
paste_integrity() {
	start_mwt
	T 'cat >~/out.txt' -k Return
	wl-copy <"$DIR/integrity.txt"
	sleep 0.5
	local t0=$SECONDS
	K v
	local p=-1 s=0 i
	for ((i = 0; i < 180; i++)); do
		s=$(stat -c %s "$HOME/out.txt" 2>/dev/null || echo 0)
		[ "$s" = "$p" ] && [ "$i" -gt 3 ] && break
		p=$s
		sleep 1
	done
	C d
	sleep 0.5
	if cmp -s "$DIR/integrity.txt" "$HOME/out.txt"; then ok "1 MB paste with long UTF-8 lines arrives byte for byte in $((SECONDS - t0))s"; else gap "paste differs: $(cmp "$DIR/integrity.txt" "$HOME/out.txt" 2>&1 | cut -c1-120), sizes $(stat -c %s "$DIR/integrity.txt") / $(stat -c %s "$HOME/out.txt")"; fi
}
paste_over_16mb() {
	start_mwt
	T 'stty -echo; cat >~/over.txt; stty echo' -k Return
	sleep 0.3
	wl-copy <"$DIR/big.txt"
	sleep 0.8
	local t0=$SECONDS
	K v
	local p=-1 s=0 i
	for ((i = 0; i < 200; i++)); do
		s=$(stat -c %s "$HOME/over.txt" 2>/dev/null || echo 0)
		[ "$s" = "$p" ] && [ "$i" -gt 8 ] && break
		p=$s
		sleep 1
	done
	C d
	sleep 0.3
	local all got
	all=$(wc -l <"$DIR/big.txt")
	got=$(wc -l <"$HOME/over.txt")
	if [ "$got" = "$all" ]; then ok "17 MB paste complete in $((SECONDS - t0))s"
	elif [ "$got" -gt 0 ]; then gap "paste over 16 MB: first $got of $all lines are SENT and the rest silently dropped ($((SECONDS - t0))s). A stalled paste is discarded whole, this one is not"
	else ok "a paste over 16 MB is dropped whole, nothing ran ($((SECONDS - t0))s)"; fi
	alive || gap "mwt died"
}
paste_bidi() {
	start_mwt
	printf 'echo \xe2\x80\xaegnp.exe' | wl-copy
	sleep 0.4
	K v
	sleep 0.3
	shown >/dev/null
	if grep -q $'\xe2\x80\xae' "$DIR/shown"; then info "U+202E from a paste stays in the input line, and mwt draws no bidi"; else info "U+202E from a paste is not on the screen"; fi
}
paste_escape() {
	start_mwt
	printf 'echo a\033[201~b\033]52\007c\x9bd' | wl-copy
	sleep 0.4
	K v; T -k Return
	if try 6 'a\[201~b\]52cd'; then ok "control bytes stripped from a paste"; else info "paste result: $(tail -3 "$DIR/shown" | cut -c1-100)"; fi
}
ctrl_c_behind_paste() {
	start_mwt
	T 'sleep 300' -k Return
	sleep 0.3
	wl-copy <"$DIR/colon.txt"
	sleep 0.6
	K v
	sleep 1
	C c
	sleep 2.5
	if pgrep -x sleep >/dev/null; then
		gap "Ctrl+C does not reach the program: the byte queues behind a paste the program never reads. 'sleep 300' still runs 2.5 s later"
	else ok "Ctrl+C works during a blocked paste"; fi
	C c
	sleep 1
	pgrep -x sleep >/dev/null && info "a second Ctrl+C does not help either"
	return 0
}
paste_holds_until_return() {
	start_mwt
	wl-copy <"$DIR/slow.txt"
	sleep 0.6
	K v
	sleep 2
	local a
	a=0
	[ -e "$HOME/ticks" ] && a=$(wc -l <"$HOME/ticks")
	if [ "$a" = 0 ]; then ok "a bracketed paste runs nothing before Return"; else gap "a bracketed paste ran $a lines before Return"; fi
	C c
	sleep 1
	T 'echo nach-paste' -k Return
	try 6 'nach-paste' && ok "Ctrl+C drops a held paste" || gap "no prompt after Ctrl+C on a held paste"
	rm -f "$HOME/ticks"
}
line_limits() {
	start_mwt
	python3 -c "print('€'*1334, end='')" | wl-copy
	sleep 0.4
	K v
	T -k Left -k Left -k Delete -k BackSpace
	T 'é'
	C w
	C a
	T -k Delete
	python3 -c "print('a'*3990, end='')" | wl-copy
	sleep 0.4
	K v
	K v
	C u
	python3 -c "print('A'*3995, end='')" | wl-copy
	sleep 0.4
	T 'echo '
	K v
	T 'ééé'
	T -k Up -k Down -k Down
	C u
	python3 -c "print('printf %s '+'B'*3970+' | wc -c')" | wl-copy
	sleep 0.4
	K v
	T -k Return
	if alive && try 8 '3970'; then ok "edits at INPUT_MAX, multibyte at the boundary; a long line runs"; else gap "line limits: $(tail -2 "$DIR/shown" | cut -c1-120)"; fi
}

# What a paste must do, whatever it looks like: the kernel echoes the lines as
# they arrive while bash already answers, so prompts and echo mix. Each line of
# output appears, which takes the 3 backslashes, and the shell has no complaint.
snippet_results() {
	local missing=0 l
	while IFS= read -r l; do
		grep -qF -- "$l" "$DIR/shown" || { missing=$((missing + 1)); gap "$1: no output '$(echo "$l" | cut -c1-60)'"; }
	done <"$DIR/snippet.out"
	[ "$missing" = 0 ] && ok "$1: all output is there, in order of the commands"
	if grep -q 'command not found\|syntax error' "$DIR/shown"; then gap "$1: shell errors: $(grep -m1 'command not found\|syntax error' "$DIR/shown" | cut -c1-70)"; fi
	local a b c
	a=$(grep -n -m1 -F 'LOGO=' "$DIR/shown" | cut -d: -f1)
	b=$(grep -n -m1 -F ':/bin/bash' "$DIR/shown" | cut -d: -f1)
	c=$(grep -n -m1 -F "$(tail -5 "$DIR/snippet.out" | head -1)" "$DIR/shown" | cut -d: -f1)
	[ "${a:-0}" -lt "${b:-0}" ] && [ "${b:-0}" -lt "${c:-0}" ] || gap "$1: the output is out of order: $a $b $c"
}
# The rows of the tall window hold the whole session, the screen is what shows.
tall() { swaymsg -q '[app_id="mwt"] resize set 970 670, move position 20 20'; sleep 0.4; C l; }
# Typed, it reads as a session should.
snippet_typed() {
	start_mwt
	tall
	local l
	while IFS= read -r l; do
		if [ -n "$l" ]; then T "$l" -k Return; else T -k Return; fi
		sleep 0.1
	done <"$DIR/snippet.sh"
	sleep 1
	shown >/dev/null
	{
		sed -n 1,2p "$DIR/snippet.sh" | sed 's/^/~$ /'
		cat /etc/os-release
		echo '~$'
		sed -n 4,5p "$DIR/snippet.sh" | sed 's/^/~$ /'
		grep -n "/bin/bash" /etc/passwd
		echo '~$'
		sed -n 7,8p "$DIR/snippet.sh" | sed 's/^/~$ /'
		sed -n 9,11p "$DIR/snippet.sh" | sed 's/^/> /'
		tail -5 "$DIR/snippet.out"
		printf '~$'
	} >"$DIR/snippet.want"
	if cmp -s "$DIR/snippet.want" "$DIR/shown"; then ok "typed: the whole session in order"; else gap "typed: not the session it should be: $(diff "$DIR/snippet.want" "$DIR/shown" | head -3 | cut -c1-90 | tr '\n' '|')"; fi
}
snippet_paste() {
	start_mwt
	tall
	wl-copy <"$DIR/snippet.sh"
	sleep 0.4
	K v
	sleep 0.5
	T -k Return
	sleep 2
	shown >/dev/null
	snippet_results "pasted, then Return"
}
snippet_paste_no_newline() {
	start_mwt
	tall
	head -c -1 "$DIR/snippet.sh" | wl-copy
	sleep 0.4
	K v
	sleep 2
	shown >/dev/null
	if tail -1 "$DIR/shown" | grep -qF "| awk '{print \$5, \$9}'" && ! grep -qxF "$(tail -1 "$DIR/snippet.out")" "$DIR/shown"; then ok "pasted without a final newline: the last line waits in the input"; else gap "pasted without a final newline: $(tail -1 "$DIR/shown" | cut -c1-70)"; fi
	T -k Return
	sleep 1
	shown >/dev/null
	snippet_results "pasted, then Return"
}

# ---- terminal modes
stty_raw_read_in_shell() {
	start_mwt
	T 'stty raw -echo; IFS= read -r -n1 k; stty sane; echo "k=[$k]"' -k Return
	sleep 2.2
	T 'q'
	if try 6 'k=\[q\]'; then ok "stty raw + read in the shell itself"; else
		gap "'stty raw; read -n1' typed in the shell: mwt 'repairs' the terminal under the running read, so the key does not arrive and the line hangs until Return"
		T -k Return
		try 5 'k=\[.*\]' && info "after Return: $(grep -x 'k=\[.*\]' "$DIR/shown" | tail -1)"
	fi
}
inlcr() {
	start_mwt
	T 'stty inlcr' -k Return
	sleep 0.4
	T 'echo inlcr-ok' -k Return
	if try 6 'inlcr-ok'; then ok "stty inlcr"; else gap "after 'stty inlcr' Return does nothing: mwt sends \\n where a terminal sends \\r, and the shell never gets a line"; fi
}
tcoff() {
	start_mwt
	T 'python3 -c "import termios;termios.tcflow(0,termios.TCOOFF)"' -k Return
	sleep 1
	T 'echo tcoff-weiter' -k Return
	if try 5 'tcoff-weiter'; then ok "output flows after TCOOFF"; return; fi
	C c; C q; sleep 0.5
	if try 4 'tcoff-weiter'; then info "TCOOFF ends on Ctrl+C/Q"; return; fi
	info "TCOOFF: output stays stopped, keys do not help (a kernel stop; only tcflow(TCOON) ends it)"
}
vkey() { local l=$1; shift; (sleep 0.6; echo "key Escape"; sleep 0.2; for k in "$@"; do echo "key $k"; sleep 0.3; done; sleep 0.3) | VIN_LAYOUT=$l "$VINPUT" 1000 700 >/dev/null 2>&1; }
layouts() { [ "${VIN_LAYOUTS:-}" = 1 ] || { info "skipped: vinput has no layouts"; return 1; }; }
cyrillic_ctrl_c() {
	layouts || return
	local l
	for l in ru ru,us gr; do
		start_mwt
		T 'sleep 300' -k Return
		sleep 0.3
		vkey "$l" ctrl+c
		sleep 0.8
		if pgrep -x sleep >/dev/null; then gap "Ctrl+C on layout '$l' does nothing: keys are compared by keysym only"; else ok "Ctrl+C on layout '$l'"; fi
		stop_mwt
	done
}
cyrillic_raw() {
	layouts || return
	start_mwt
	T 'python3 -c "import tty,time;tty.setcbreak(0);time.sleep(300)"' -k Return
	sleep 1
	vkey ru ctrl+c
	sleep 0.8
	if pgrep -f time.sleep >/dev/null; then gap "Ctrl+C on a ru layout does not reach a program in cbreak mode"; pkill -f time.sleep; else ok "Ctrl+C on ru layout in cbreak mode"; fi
}
copy_on_ru() {
	layouts || return
	start_mwt
	T 'echo kopie-ru' -k Return
	echo vorher | wl-copy
	sleep 0.3
	sel_screen
	vkey ru ctrl+shift+c
	if timeout 2 wl-paste -n | grep -qx 'kopie-ru'; then ok "Ctrl+Shift+C on a ru layout"; else gap "Ctrl+Shift+C on a ru layout does nothing"; fi
}
raw_paste_ctrl_c() {
	start_mwt
	T 'python3 -c "import tty,time;tty.setcbreak(0);time.sleep(300)"' -k Return
	sleep 1
	wl-copy <"$DIR/colon.txt"
	sleep 0.6
	K v
	sleep 1
	C c
	sleep 1.5
	if pgrep -f time.sleep >/dev/null; then gap "cbreak program: Ctrl+C stuck behind a paste"; pkill -f time.sleep; else ok "cbreak program: Ctrl+C gets through a blocked paste"; fi
}
python_repl() {
	start_mwt
	T 'python3' -k Return
	sleep 1
	T 'print(6*7)' -k Return
	if try 6 '.*42'; then ok "python REPL"; else gap "python REPL: $(tail -3 "$DIR/shown" | tr '\n' '|' | cut -c1-120)"; fi
	C d
	sleep 0.5
	T 'echo repl-ende' -k Return
	try 6 'repl-ende' || gap "shell did not come back after the REPL"
}
flood_ctrl_c() {
	start_mwt
	T 'yes' -k Return
	sleep 2
	local t0
	t0=$(date +%s%N)
	C c
	local i
	for ((i = 0; i < 100; i++)); do pgrep -x yes >/dev/null || break; sleep 0.05; done
	info "yes stopped $((($(date +%s%N) - t0) / 1000000)) ms after Ctrl+C"
	sleep 1
	T 'echo flood-ende' -k Return
	try 10 'flood-ende' || gap "no prompt after the flood"
}
# Queries answered faster than the program reads: the answers wait in mwt. These
# two measure memory, which ASan's quarantine of freed blocks would otherwise fill.
SMALL_QUARANTINE=ASAN_OPTIONS=detect_leaks=0:quarantine_size_mb=1
reply_flood() {
	start_mwt "$SMALL_QUARANTINE"
	local pid r0 r1
	pid=$(mwtpid)
	T 'stty -echo; yes $'"'"'\033[c'"'"'' -k Return
	sleep 8
	r0=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	sleep 20
	r1=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	info "RSS while answers pile up: $r0 kB at 8 s, $r1 kB at 28 s"
	[ "$((r1 - r0))" -gt 51200 ] && gap "answers to a program that does not read pile up without limit: $r0 kB to $r1 kB in 20 s"
	C c
	local i
	for ((i = 0; i < 60; i++)); do pgrep -x yes >/dev/null || break; sleep 0.05; done
	if pgrep -x yes >/dev/null; then gap "Ctrl+C does not stop a query flood"; else ok "Ctrl+C stops a query flood"; fi
	sleep 0.5
	T 'stty echo; echo flood-weg' -k Return
	try 10 'flood-weg' && ok "a prompt after the query flood" || gap "no prompt after the query flood: $(tail -2 "$DIR/shown" | cut -c1-70 | tr '\n' '|')"
}
# The same without signals: Ctrl+C is a plain byte that waits with the rest.
reply_flood_raw() {
	start_mwt "$SMALL_QUARANTINE"
	local pid r0 r1
	pid=$(mwtpid)
	T 'python3 -c "import os,tty;tty.setraw(0);b=b\"\\033[c\"*4096;exec(\"while 1: os.write(1,b)\")"' -k Return
	sleep 8
	r0=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	sleep 20
	r1=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	info "RSS with a raw program: $r0 kB at 8 s, $r1 kB at 28 s"
	[ "$((r1 - r0))" -gt 51200 ] && gap "answers to a raw program that does not read pile up without limit: $r0 kB to $r1 kB in 20 s"
	swaymsg -q '[app_id="mwt"] kill'
	sleep 3
	if alive; then gap "the window cannot be closed during a query flood"; else ok "the window closes during a query flood"; fi
	pkill -f 'os.write' 2>/dev/null
	return 0
}
flood_rss() {
	start_mwt
	local pid r0 r1
	pid=$(mwtpid)
	T 'yes "äöü 漢字 $(printf x%.0s {1..60})"' -k Return
	sleep 10
	r0=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	sleep 25
	r1=$(awk '/VmRSS/{print $2}' /proc/$pid/status)
	C c
	info "RSS during a flood: $r0 kB at 10 s, $r1 kB at 35 s; CPU $(ps -o %cpu= -p "$pid")%"
}

# ---- start up
args() {
	LOG=$DIR/log.args
	local rc f
	: >"$LOG"
	timeout 3 "$MWT" -g 4096x2048 -e sleep 30 >>"$LOG" 2>&1
	rc=$?
	if [ "$rc" = 2 ]; then ok "-g with a buffer over 2 GiB is refused"; else gap "-g 4096x2048 ends with code $rc instead of 2"; fi
	: >"$LOG"
	for f in 'monospace:size=100000' 'monospace:size=0.01' 'monospace:size=-3' 'Nichtda:size=11' 'a:b' 'monospace:bold:italic:size=1e9' "$(printf 'x%.0s' {1..5000})"; do
		timeout 3 "$MWT" -f "$f" -e sleep 30 >>"$LOG" 2>&1
		rc=$?
		info "-f '$(echo "$f" | cut -c1-30)': $([ "$rc" = 124 ] && echo runs || echo "ends with code $rc")"
		[ "$rc" -gt 128 ] && gap "-f '$(echo "$f" | cut -c1-30)' killed mwt: code $rc"
		check_log
		: >"$LOG"
	done
	for f in 1x1 500x1 1x500 2000x2000 99999x99999 100000000x100000000 2147483647x2147483647 0x0 -1x-1 x; do
		timeout 3 "$MWT" -g "$f" -e sleep 30 >>"$LOG" 2>&1
		rc=$?
		info "-g $f: $([ "$rc" = 124 ] && echo runs || echo "ends with code $rc")"
		[ "$rc" -gt 128 ] && gap "-g $f killed mwt: code $rc"
		check_log
		: >"$LOG"
	done
}
shells() {
	LOG=$DIR/log.shells
	local s rc
	start_mwt SHELL=
	T 'echo shell-da' -k Return
	if try 5 'shell-da'; then ok "an empty SHELL falls back to the passwd entry"; else gap "SHELL='': no usable shell"; fi
	stop_mwt
	for s in /nichtda bash; do
		start_mwt SHELL="$s"
		alive && info "SHELL='$s': the window stays" || info "SHELL='$s': the window closes at once"
		stop_mwt
	done
	"$MWT" -e /nichtda/prog >"$LOG" 2>&1
	rc=$?
	info "mwt -e of a missing program: code $rc, stderr '$(head -1 "$LOG" | cut -c1-60)'"
	"$MWT" -e sh -c 'exit 7' >"$LOG" 2>&1
	rc=$?
	info "mwt -e of 'exit 7': code $rc"
	: >"$LOG"
}
# With no terminfo mwt warns on stderr. Closed at the start, fd 2 may by then be
# the Wayland socket, and the message must not end up in it.
closed_stdio() {
	LOG=$DIR/log.closed
	: >"$LOG"
	mkdir -p "$DIR/noterm"
	cp "$HOME/.bashrc" "$DIR/noterm/"
	(cd "$HOME" && HOME=$DIR/noterm sh -c 'exec "$0" 2>&- >&- <&- &' "$MWT")
	sleep 1.5
	T 'echo stdio-ok' -k Return
	if alive && try 5 'stdio-ok'; then ok "closed stdio with a message for stderr"; else gap "started with stderr closed, its own stderr message goes into the Wayland socket (fd 2): alive=$(alive && echo y || echo n)"; fi
}
cwd_tricks() {
	mkdir -p "$HOME/weg"
	(cd "$HOME/weg" && rmdir "$HOME/weg" && sh -c '"$0" >"$1" 2>&1 &' "$MWT" "$DIR/log.$CASE")
	LOG=$DIR/log.$CASE
	sleep 1.5
	T 'echo im-nichts' -k Return
	try 5 'im-nichts' && ok "started in a deleted directory" || gap "deleted cwd"
}
exit_with_jobs() {
	start_mwt
	T '(while :; do echo x; sleep 0.01; done) & sleep 1000 & exit' -k Return
	sleep 3
	if alive; then gap "mwt stays after the shell exits with jobs on the pty"; else ok "mwt ends with the shell, jobs on the pty or not"; fi
	pgrep -x sleep >/dev/null && info "a background 'sleep 1000' outlives the window (HUP not delivered to it)"
	pkill -f 'while :' 2>/dev/null
}
close_ignores_hup() {
	start_mwt
	T "exec sh -c \"trap '' HUP; while :; do sleep 1; done\"" -k Return
	sleep 0.5
	local pid
	pid=$(mwtpid)
	swaymsg -q '[app_id="mwt"] kill'
	sleep 0.5
	swaymsg -t get_tree | grep -q '"app_id": "mwt"' && gap "the window stays up while mwt waits for the shell"
	sleep 3.5
	if kill -0 "$pid" 2>/dev/null; then gap "mwt runs on without a window: its shell ignores SIGHUP"; else ok "a shell that ignores SIGHUP does not keep mwt after the window is closed"; fi
	if pgrep -f "trap '' HUP" >/dev/null; then ok "the shell that ignores SIGHUP is not killed"; else gap "mwt killed a shell that ignores SIGHUP"; fi
	pkill -9 -f "trap '' HUP" 2>/dev/null
	return 0
}

# ---- window
tiny() {
	start_mwt
	swaymsg -q '[app_id="mwt"] resize set 1 1, move position 20 20'
	sleep 0.5
	T 'python3 -c "print(\"漢字🙂é\"*200)"' -k Return
	sleep 0.5
	python3 -c "print('漢'*1300, end='')" | wl-copy
	sleep 0.4
	K v
	T -k Home -k End -k Left
	sel_screen; K c
	v move 40 40; v click left; v click left; v click left
	C u
	C k
	# sway keeps the middle of a window it resizes, here far past the left edge
	swaymsg -q '[app_id="mwt"] resize set 800 500, move position 20 20'
	sleep 0.4
	T 'stty size' -k Return
	if alive && try 6 '[0-9]* [0-9]*'; then ok "smallest window with wide characters and a 4000 byte line"; else gap "tiny window: alive=$(alive && echo y || echo n), tail: $(tail -3 "$DIR/shown" | cut -c1-70 | tr '\n' '|')"; fi
}
resize_storm() {
	start_mwt
	T 'yes "漢字 äöü é $(printf x%.0s {1..120})"' -k Return
	local i
	for ((i = 0; i < 80; i++)); do
		swaymsg -q "[app_id=\"mwt\"] resize set $((1 + RANDOM % 980)) $((1 + RANDOM % 680))" 2>/dev/null
		alive || break
	done
	C c
	sleep 1
	if alive; then ok "80 resizes through a flood"; else gap "mwt died in a resize storm"; fi
}
fd_leak() {
	start_mwt
	local pid a b i
	pid=$(mwtpid)
	a=$(ls /proc/"$pid"/fd | wc -l)
	for ((i = 0; i < 25; i++)); do
		echo "leak $i" | wl-copy
		sleep 0.1
		K v
		sel_screen; K c
		timeout 2 wl-paste -n >/dev/null
		timeout 2 wl-paste -t STRING >/dev/null
		v move 100 100; v click middle
		C u
	done
	b=$(ls /proc/"$pid"/fd | wc -l)
	if [ "$b" -gt $((a + 3)) ]; then gap "file descriptors: $a -> $b after 25 paste/copy rounds"; else ok "file descriptors stay at $a -> $b"; fi
}
stalled_readers() {
	start_mwt
	T "seq -f '%078g' 1 6000" -k Return
	sleep 4
	sel_history
	K c
	local i
	for ((i = 0; i < 14; i++)); do (wl-paste -n | sleep 9) & done
	sleep 1
	T 'echo responsive' -k Return
	if try 6 'responsive'; then ok "14 readers that do not read"; else gap "stalled clipboard readers block mwt"; fi
	sleep 7
}
# wl-paste reads everything into a file of its own first and never stalls, so
# stalled_readers cannot catch a source that writes blocking. stallreader can.
stalled_pipes() {
	[ -n "$STALL" ] || { info "stallreader did not build"; return 0; }
	start_mwt
	T "seq -f '%078g' 1 6000" -k Return
	sleep 4
	sel_history
	K c
	local want got sp
	want=$(timeout 5 wl-paste -n | wc -c)
	"$STALL" 3 60 >"$DIR/stall.out" 2>&1 &
	sp=$!
	sleep 1
	grep -q stalled "$DIR/stall.out" || info "stallreader: $(head -1 "$DIR/stall.out")"
	T 'echo responsive' -k Return
	if try 3 'responsive'; then ok "3 readers that ask for $want bytes and never read"; else gap "a clipboard reader that does not read blocks mwt"; fi
	got=$(timeout 5 wl-paste -n | wc -c)
	if [ "$got" = "$want" ]; then ok "the clipboard is served whole next to them ($got bytes)"; else gap "clipboard next to stalled readers: $got of $want bytes"; fi
	# still within the readers' 60 s, so nothing above got through by their exit
	kill -0 "$sp" 2>/dev/null || info "stallreader ended early, the checks above prove less"
	kill "$sp" 2>/dev/null
	wait "$sp" 2>/dev/null
}
menu_abuse() {
	start_mwt
	T 'yes menu' -k Return
	local i
	for ((i = 0; i < 24; i++)); do
		v move 300 200
		v click right
		sleep 0.1
		case $((i % 4)) in
		0) wtype -k Escape ;;
		1) v click left ;;
		2) swaymsg -q '[app_id="mwt"] resize set 500 300' ;;
		3) K plus ;;
		esac
		alive || break
	done
	# The last round can leave the menu open, and it would take the Ctrl+C.
	T -k Escape
	C c
	sleep 0.5
	if alive; then ok "the menu through a flood, a resize and a zoom"; else gap "the context menu killed mwt"; fi
	pgrep -x yes >/dev/null && gap "Ctrl+C does not reach the flood after the menu closed"
	T 'sleep 1; exit' -k Return
	v move 300 200
	v click right
	sleep 2
	if alive; then gap "mwt stays when the shell ends behind an open menu"; else ok "the shell ends behind an open menu"; fi
}
compositor_dies() {
	start_mwt
	T 'sleep 777' -k Return
	sleep 0.5
	kill -9 "$SWAY"
	sleep 2
	if alive; then gap "mwt survives its compositor"; else ok "mwt ends with the compositor: $(tail -1 "$LOG" | cut -c1-90)"; fi
	pgrep -f 'sleep 777' >/dev/null && gap "the foreground program is orphaned when the compositor dies"
	pkill -x sleep
	start_sway
}

for c in dcs_swallow osc_swallow_cat fuzz noncharacters \
	mid_char_selection drag_during_flood select_history click_clusters \
	paste_integrity paste_bidi paste_escape ctrl_c_behind_paste paste_holds_until_return line_limits paste_over_16mb \
	snippet_typed snippet_paste snippet_paste_no_newline \
	stty_raw_read_in_shell inlcr tcoff cyrillic_ctrl_c cyrillic_raw copy_on_ru raw_paste_ctrl_c python_repl flood_ctrl_c flood_rss reply_flood reply_flood_raw \
	args shells closed_stdio cwd_tricks exit_with_jobs close_ignores_hup \
	tiny resize_storm fd_leak stalled_readers stalled_pipes menu_abuse compositor_dies; do
	run "$c"
done
echo "=== $GAPS gaps, $CRASHES crashes ==="
[ "$GAPS" = 0 ] && [ "$CRASHES" = 0 ]
