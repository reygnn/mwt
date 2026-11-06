#!/bin/bash
# Black-box test of mwt under a headless sway: keys from wtype, the mouse from
# mwc's tests/vinput, and what mwt shows read back the way a user gets at it:
# a drag over the window, then the primary selection through wl-paste. That only
# sees the screen, not the scrollback.
#
# Needs sway, wtype, wl-clipboard (wl-copy, wl-paste), grim, tic and vinput. Its
# own runtime directory and socket, so a running session is not touched. Goes on
# after a failed check and fails at the end.
set -u
cd "$(dirname "$0")/.." || exit 1
MWT=${MWT:-$PWD/mwt}
VINPUT=${VINPUT:-${MWC_DIR:-../../mwc}/build/vinput}
INFO=$PWD/mwt.info
TIMEOUT=10
FAILS=0

for t in sway wtype wl-copy wl-paste grim tic python3; do
	command -v "$t" >/dev/null || { echo "FAIL: $t not installed"; exit 1; }
done
[ -x "$MWT" ] || { echo "FAIL: $MWT not built"; exit 1; }
[ -x "$VINPUT" ] || { echo "FAIL: $VINPUT not built (make build/vinput in mwc)"; exit 1; }

DIR=$(mktemp -d /tmp/mwt-test.XXXXXX) || exit 1
LOG=$DIR/log
mkdir -p "$DIR/home"
export XDG_RUNTIME_DIR=$DIR HOME=$DIR/home SHELL=/bin/bash
export XDG_CONFIG_HOME=$DIR/config
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman
export LANG=C.UTF-8 LC_ALL=C.UTF-8
unset WAYLAND_DISPLAY DISPLAY TERMINFO TERMINFO_DIRS
# Programs that take the terminal raw and put it back: bash keeps what they leave.
cat >"$HOME/.bashrc" <<'RC'
PS1="\w\$ "
unset PROMPT_COMMAND
RAW='import termios,tty,sys; a=termios.tcgetattr(0); tty.setraw(0); c=sys.stdin.read(%d); termios.tcsetattr(0,termios.TCSANOW,a); %s'
RAW3=$(printf "$RAW" 3 'sys.stdout.write(repr(c)+chr(10))')
RAW1=$(printf "$RAW" 1 'sys.stdout.write(c)')
RC
tic -x -o "$HOME/.terminfo" "$INFO" || { echo "FAIL: mwt.info does not compile"; exit 1; }
cat >"$DIR/sway.conf" <<'CONF'
output HEADLESS-1 resolution 1000x700 position 0 0
default_border none
default_floating_border none
for_window [app_id="mwt"] floating enable, move position 20 20
CONF

SWAY=
VIN_PID=
cleanup() {
	[ -n "$VIN_PID" ] && kill "$VIN_PID" 2>/dev/null
	pkill -f "^$MWT" 2>/dev/null
	[ -n "$SWAY" ] && kill "$SWAY" 2>/dev/null
	wait 2>/dev/null
	rm -rf "$DIR"
}
trap cleanup EXIT

die() {
	echo "FAIL: $1"
	sed 's/^/log: /' "$LOG" 2>/dev/null
	exit 1
}
fail() {
	FAILS=$((FAILS + 1))
	echo "FAIL: $1"
	tail -4 "$DIR/shown" 2>/dev/null | cut -c1-100 | sed 's/^/  shown: /'
}

sway -c "$DIR/sway.conf" >"$DIR/sway.log" 2>&1 &
SWAY=$!
for ((i = 0; i < 50; i++)); do
	[ -S "$DIR/wayland-1" ] && break
	sleep 0.1
done
[ -S "$DIR/wayland-1" ] || die "sway did not start"
export WAYLAND_DISPLAY=wayland-1
sleep 0.3
export SWAYSOCK=$(ls "$DIR"/sway-ipc.*.sock 2>/dev/null | head -1)

coproc VIN { "$VINPUT" 1000 700; }
VIN_PID=$VIN_PID
# Bash closes a coproc's descriptors in subshells, and `shown | grep` is one.
exec 7>&"${VIN[1]}" 8<&"${VIN[0]}"
v() { echo "$*" >&7; read -r -t 3 _ <&8; }

# A new virtual keyboard per wtype call: sway hands its first key over in
# wl_keyboard.enter, which a client must not take as a press. So each call
# starts with a key mwt ignores.
T() { wtype -k Shift_L "$@"; sleep 0.3; }
K() { wtype -k Shift_L -M ctrl -M shift -k "$1" -m shift -m ctrl; sleep 0.3; }
C() { wtype -k Shift_L -M ctrl -k "$1" -m ctrl; sleep 0.3; }
# Shift held by a wtype in the background, while vinput clicks.
shift_click() { wtype -k Shift_L -M shift -s 800 -m shift & sleep 0.3; v click "${1:-left}"; wait $!; sleep 0.1; }

# What mwt shows, without the blank rows at the end. The pause keeps two drags
# from counting as a double click.
shown() {
	sleep 0.45
	v move 21 21
	v down left
	v move 999 699
	v up left
	sleep 0.1
	printf '%s' "$(timeout 3 wl-paste -p -n 2>/dev/null)" >"$DIR/shown"
	cat "$DIR/shown"
}

# expect <description> <grep -x pattern>: a line like that shows up
expect() {
	local i
	for ((i = 0; i < TIMEOUT; i++)); do
		shown | grep -qx -- "$2" && { echo "ok: $1"; return 0; }
		sleep 0.2
	done
	fail "$1: no line \"$2\""
}

# expect_n <description> <grep -x pattern> <n>: n lines like that
expect_n() {
	local i
	for ((i = 0; i < TIMEOUT; i++)); do
		[ "$(shown | grep -cx -- "$2")" = "$3" ] &&
			{ echo "ok: $1"; return 0; }
		sleep 0.2
	done
	fail "$1: not $3 lines \"$2\""
}

# Started the way an autostart script does it: `mwt &` from sh, which makes
# mwt ignore SIGINT and SIGQUIT. The shell in it must not.
start_mwt() {
	(cd "$HOME" && sh -c '"$0" >>"$1" 2>&1 &' "$MWT" "$LOG")
	sleep 1.5
	pgrep -f "^$MWT" >/dev/null || die "mwt did not start"
}
start_mwt

# The grid in pixels, from the window; px gives the middle of a cell in X and Y.
read -r W H < <(swaymsg -t get_tree | python3 -c '
import json, sys
def walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from walk(c)
for n in walk(json.load(sys.stdin)):
    if n.get("app_id") == "mwt":
        print(n["rect"]["width"], n["rect"]["height"])
        break')
[ -n "${W:-}" ] || die "no mwt window in sway's tree"
CW=$(((W - 4) / 80))
CH=$(((H - 4) / 24))
px() { X=$((22 + $1 * CW + CW / 2)); Y=$((22 + $2 * CH + CH / 2)); }
at() { px "$1" "$2"; v move "$X" "$Y"; }

T 'echo $TERM' -k Return
expect "the terminfo entry is used" 'mwt'

T 'echo hallo-$((6*7)) äöü' -k Return
expect "a command runs, UTF-8 in and out" 'hallo-42 äöü'

T 'printf "abc\bX\n12345\r9\n"' -k Return
expect "backspace overwrites" 'abX'
expect "carriage return overwrites" '92345'

T 'printf "\033[1;31mrot\033[0m \033]0;t\007ok\n"' -k Return
expect "escape sequences are dropped" 'rot ok'

T 'printf "a\033[Kb\rxyz\033[2Gq\033[K\n"' -k Return
expect "erase in line and cursor column" 'xq'

T 'stty size' -k Return
expect "the pty knows the size" '24 80'

T 'printf "%0200d\n" 0 | tr 0 x' -k Return
expect "a wrapped line copies as one" "$(printf '%0200d' 0 | tr 0 x)"

printf 'echo eins\necho zwei' | wl-copy
sleep 0.3
K v
T -k Return
expect "paste, first line sent" 'eins'
expect "paste, second line sent on Return" 'zwei'

T 'read -s -p "pw: " x; echo; echo "got:$x"' -k Return
T 'geheim'
if shown | grep -q geheim; then fail "the password shows while typed"; else echo "ok: a password does not show"; fi
T -k Return
expect "the password reaches the program" 'got:geheim'

T 'python3 -c "$RAW3"' -k Return
sleep 0.8
T 'xyz'
expect "raw mode sends keys at once" "'xyz'"
T 'echo wieder-normal' -k Return
expect "the shell gets its line discipline back" 'wieder-normal'

T 'sleep 3017' -k Return
C c
if pgrep -f '^sleep 3017$' >/dev/null; then
	fail "Ctrl+C does not interrupt"
	pkill -f '^sleep 3017$'
	sleep 0.3
else
	T 'echo status $?' -k Return
	expect "Ctrl+C interrupts" 'status 130'
fi

T 'read -n 1 -p "key: " k; echo " k=$k"' -k Return
T 'q'
expect "read -n 1 keeps its raw mode" 'key: q k=q'

T 'cat' -k Return
T 'zeile' -k Return
C d
T 'echo cat-ende' -k Return
expect "Ctrl+D ends input" 'cat-ende'

C l
T 'echo eins-zwei' -k Return
T 'echo drei' -k Return
T -k Up -k Up -k Return
expect_n "history" 'eins-zwei' 2

# ---- regressions from mt's reviews ----------------------------------------

T 'printf "a\033[2147483647Cx\033[99999999999999999999Dy\n"' -k Return
expect "huge CSI parameters are clamped" 'y *x'

T 'grep SigIgn /proc/self/status' -k Return
expect "children do not inherit ignored signals" 'SigIgn: *0\{16\}'

T 'sleep 1; read -n 1 -p "key: " k; echo " k=$k"' -k Return
sleep 1.8
T 'q'
expect "read -n 1 after a job keeps its raw mode" 'key: q k=q'

T 'k=$(python3 -c "$RAW1"); echo "k=[$k]"' -k Return
sleep 2
T 'q'
expect "a raw program in \$(...) keeps its raw mode" 'k=\[q\]'

T 's=$(date +%s%N); python3 -c "import sys; sys.stdout.write(\"e\"+\"́\"*300000+\"\n\")"; echo dauer=$(( ($(date +%s%N) - s) / 1000000 ))' -k Return
expect "combining marks are cheap" 'dauer=[0-9]*'
ms=$(grep -o 'dauer=[0-9]*' "$DIR/shown" | tail -1 | cut -d= -f2)
[ "${ms:-99999}" -lt 3000 ] || fail "300000 combining marks took ${ms} ms"
T 'printf e; python3 -c "import sys; sys.stdout.write(\"⃐\"*3005)"' -k Return
T 'echo sichtbar'
expect "the line shows after a row of marks" '.*~\$ echo sichtbar'
T -k Return

# mwt does not reflow: what no longer fits is cut off and the cursor stays in the
# last column, so ESC[1K must clear the whole row.
size="$W $H"
swaymsg -q '[app_id="mwt"] resize set 1000 460'
sleep 0.5
C l
T 'printf "%0100d" 0; sleep 1.5; printf "\033[1K|\n"' -k Return
sleep 0.3
swaymsg -q "[app_id=\"mwt\"] resize set $size, move position 20 20"
expect "ESC[1K after the window shrank" ' *|'
C l
T 'stty size' -k Return
expect "back to 80 columns" '24 80'

C l
T 'echo äöü-string' -k Return
expect "copied" 'äöü-string'
sleep 0.45
at 0 1
v down left
at 30 1
v up left
K c
if [ "$(timeout 2 wl-paste -n)" = 'äöü-string' ]; then echo "ok: Ctrl+Shift+C copies"; else fail "Ctrl+Shift+C does not copy"; fi
if timeout 2 wl-paste -t STRING | iconv -f latin1 -t utf-8 | grep -qx 'äöü-string'; then
	echo "ok: STRING is Latin-1"
else
	fail "STRING is not Latin-1"
fi
printf 'echo caf\xe9' | wl-copy -t STRING
sleep 0.3
K v
T -k Return
expect "a Latin-1 paste is converted" 'café'

T 'printf "%080d\bX\n" 0' -k Return
expect "backspace on a full row" '0\{78\}X0'

T "printf '\\033]0;bad\\xffdir\\007'" -k Return
sleep 0.5
if swaymsg -t get_tree 2>/dev/null | python3 -c '
import json, sys
def walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from walk(c)
t = json.load(sys.stdin.buffer)
names = [n.get("name") or "" for n in walk(t) if n.get("app_id") == "mwt"]
sys.exit(0 if any(x.startswith("bad") and x.endswith("dir") for x in names) else 1)'; then
	echo "ok: the title is valid UTF-8"
else
	fail "the title is not cleaned up"
fi

# ---- unterminated strings: a real terminal waits for the terminator, and the
# escape sequence bash sends with its next prompt is what ends them.

T 'printf "\033Pverloren\n"' -k Return
T 'echo nach-dcs' -k Return
expect "an unterminated DCS ends with the next escape" 'nach-dcs'
printf 'text\033]0;ohne ende\nzeile2\n' >"$HOME/stray"
T 'cat ~/stray' -k Return
T 'echo nach-osc' -k Return
expect "a stray OSC in a file does not swallow the rest" 'nach-osc'

T 'stty inlcr' -k Return
sleep 0.3
T 'stty -inlcr; echo nach-inlcr' -k Return
expect "Return works after stty inlcr" 'nach-inlcr'

# ---- Tab belongs to the shell and the program -----------------------------

mkdir -p "$HOME/tabs/unterdir"
T 'cd tabs' -k Return
T 'ls -d unt' -k Tab -k Return
expect "Tab reaches the shell's completion" 'unterdir/'
C l
T 'cat' -k Return
T 'ab' -k Tab 'c' -k Return
expect_n "in a program, Tab moves to the next stop" 'ab *c' 2
C d
T 'cd' -k Return

# ---- mouse ----------------------------------------------------------------

C l
sleep 0.2
T 'echo alpha beta gamma' -k Return
sleep 0.6
at 7 1
v click left
v click left
sleep 0.3
[ "$(timeout 2 wl-paste -p -n)" = beta ] && echo "ok: double click selects a word" ||
	fail "double click selects a word: '$(timeout 2 wl-paste -p -n)'"
sleep 0.45
at 13 1
shift_click
sleep 0.3
[ "$(timeout 2 wl-paste -p -n)" = 'beta gamma' ] && echo "ok: Shift+click extends" ||
	fail "Shift+click extends: '$(timeout 2 wl-paste -p -n)'"
sleep 0.45
C l
T 'echo alpha beta gamma' -k Return
sleep 0.6
at 7 0
v click left
v click left
v click left
sleep 0.3
[ "$(timeout 2 wl-paste -p -n)" = '~$ echo alpha beta gamma' ] && echo "ok: triple click selects a line" ||
	fail "triple click selects a line: '$(timeout 2 wl-paste -p -n)'"
sleep 0.45
C l
T 'echo '
at 40 10
v click middle
T -k Return
expect "middle click pastes the primary selection" '~\$ echo alpha beta gamma'
# reading the screen took over the primary selection
C l
T 'echo alpha beta gamma' -k Return
sleep 0.6
at 7 0
v click left
v click left
v click left
sleep 0.6
C l
T 'echo '
wtype -k Shift_L -M shift -k Insert -m shift
sleep 0.3
T -k Return
expect "Shift+Insert pastes the primary selection" '~\$ echo alpha beta gamma'

# Shift+Home and Shift+End jump through the scrollback, Shift+wheel goes by pages.
SH() { wtype -k Shift_L -M shift -k "$1" -m shift; sleep 0.3; }
T 'clear' -k Return
T 'seq -f "zeile-%g" 1 300' -k Return
sleep 0.5
SH Home
expect "Shift+Home goes to the top of the scrollback" 'zeile-1'
SH End
expect "Shift+End goes back to the end" 'zeile-300'
at 5 5
wtype -k Shift_L -M shift -s 1000 -m shift &
sleep 0.4
v wheel -1
wait $!
expect "Shift+wheel goes up a page" 'zeile-255'
expect_n "and not further" 'zeile-254' 0
SH End
# Programs that take the screen or the mouse keep the keys.
for m in 1049 1000; do
	T "printf '\\033[?${m}h'; cat -v" -k Return
	SH Home
	T -k Return
	expect "with mode $m Shift+Home goes to the program" '\^\[\[1;2H'
	C c
	T "printf '\\033[?${m}l'" -k Return
done

# Ctrl+Shift+A selects the scrollback and the screen, without the empty rows below.
T 'clear' -k Return
T 'seq -f "zeile-%g" 1 300' -k Return
sleep 0.5
K a
K c
sleep 0.3
timeout 3 wl-paste -n >"$DIR/all"
n=$(grep -c '^zeile-' "$DIR/all")
if [ "$n" = 300 ] && grep -qx 'zeile-1' "$DIR/all" && [ "$(tail -n 1 "$DIR/all")" = '~$' ]; then
	echo "ok: Ctrl+Shift+A selects everything"
else
	fail "Ctrl+Shift+A: $n of 300 lines, the last is '$(tail -n 1 "$DIR/all")'"
fi
timeout 3 wl-paste -p -n | cmp -s - "$DIR/all" && echo "ok: and sets the primary selection" || fail "Ctrl+Shift+A: the primary selection differs"
T "printf '\\033[?1049hnur-alt\\n'" -k Return
sleep 0.3
K a
K c
sleep 0.3
timeout 3 wl-paste -n >"$DIR/all"
if grep -qx 'nur-alt' "$DIR/all" && ! grep -q '^zeile-' "$DIR/all"; then echo "ok: on the alternate screen only the screen"; else fail "Ctrl+Shift+A on the alternate screen: $(wc -l <"$DIR/all") lines"; fi
T "printf '\\033[?1049l'" -k Return
T "seq -f '%078g' 1 6000" -k Return
sleep 3
K a
K c
sleep 0.5
timeout 5 wl-paste -n >"$DIR/all"
n=$(wc -l <"$DIR/all")
if [ "$n" -ge 5000 ] && grep -q '0006000$' "$DIR/all"; then echo "ok: Ctrl+Shift+A with a full scrollback: $n lines"; else fail "Ctrl+Shift+A with a full scrollback: $n lines"; fi

# The context menu opens below and right of the pointer: a 1 px border, then rows
# of a cell and twice the padding. Copy, Paste, Select all.
menu_at() { at "$1" "$2"; v click right; sleep 0.4; }
item() { v move $((X + 30)) $((Y + 2 + (CH + 4) * $1 + (CH + 4) / 2)); sleep 0.2; }
C l
T 'echo menu-text' -k Return
sleep 0.4
at 2 1
v click left
v click left
sleep 0.5
menu_at 40 5
T -k Down -k Return
sleep 0.3
[ "$(timeout 2 wl-paste -n)" = menu-text ] && echo "ok: menu by keyboard: Copy" ||
	fail "menu by keyboard: Copy gave '$(timeout 2 wl-paste -n)'"
menu_at 40 5
item 1
v click left
sleep 0.3
expect "menu by mouse: Paste" '~\$ menu-text'
C u
menu_at 40 5
T -k Escape
T 'echo nach-esc' -k Return
expect "the right click alone runs nothing, Escape closes" 'nach-esc'
menu_at 40 5
at 60 20
v click left
sleep 0.3
T 'echo nach-klick' -k Return
expect "a click next to the menu closes it" 'nach-klick'
menu_at 40 5
item 2
v click left
sleep 0.3
timeout 2 wl-paste -p -n >"$DIR/all"
grep -qx 'menu-text' "$DIR/all" && grep -qx 'nach-klick' "$DIR/all" && echo "ok: menu: Select all" ||
	fail "menu: Select all gave $(wc -l <"$DIR/all") lines"
at 60 20
sleep 0.5
v click left
sleep 0.5
printf keep | wl-copy
sleep 0.3
menu_at 40 5
item 0
v click left
sleep 0.3
T -k Escape
[ "$(timeout 2 wl-paste -n)" = keep ] && echo "ok: menu: Copy without a selection does nothing" ||
	fail "menu: Copy without a selection gave '$(timeout 2 wl-paste -n)'"
# With the mouse taken a drag reads nothing back, so cat writes to a file.
T "printf '\\033[?1000;1006h'; cat -v >'$DIR/mm'" -k Return
sleep 0.3
at 40 5
v click right
sleep 0.2
T -k Return
sleep 0.3
grep -q '\^\[\[<2;' "$DIR/mm" && echo "ok: with the mouse taken, a right click goes to the program" ||
	fail "with the mouse taken, the program got: '$(cat "$DIR/mm")'"
at 40 5
shift_click right
sleep 0.3
T 'x' -k Escape
T 'y' -k Return
sleep 0.3
[ "$(tail -n 1 "$DIR/mm")" = y ] && echo "ok: and Shift+right click opens the menu, which takes the keys" ||
	fail "Shift+right click: the last line is '$(tail -n 1 "$DIR/mm")'"
C c
T "printf '\\033[?1000;1006l'" -k Return

# Dragging below the window selects what shows, and one row of autoscroll.
C l
T 'seq 1 200' -k Return
sleep 0.5
wtype -k Shift_L -M shift -k Prior -m shift
sleep 0.3
at 0 0
v down left
v move 999 699
v up left
sleep 0.3
first=$(timeout 2 wl-paste -p -n | head -1)
last=$(timeout 2 wl-paste -p -n | grep -xE '[0-9]+' | tail -1)
n=$(timeout 2 wl-paste -p -n | wc -l)
if [ "${n:-0}" -ge 22 ] && [ "${n:-0}" -le 25 ] && [ -n "$first" ] && [ "$last" -gt "$first" ]; then
	echo "ok: dragging below the window selects what shows"
else
	fail "dragging below the window: $n lines, '$first' to '$last'"
fi
T 'echo strg-l-weg' -k Return
expect "text to clear" 'strg-l-weg'
C l
expect_n "Ctrl+L clears the screen" 'strg-l-weg' 0
T 'clear' -k Return

# Whatever TERM mwt picks, programs must find its entry. It once set TERM=mwt for
# an entry in /usr/local, which ncurses does not search.
mkdir -p "$DIR/noinfo"
HOME=$DIR/noinfo "$MWT" -e sh -c "tput clear >/dev/null && echo \"\$TERM\" >'$DIR/term'" 2>>"$LOG"
if [ -s "$DIR/term" ]; then echo "ok: without ~/.terminfo tput knows TERM=$(cat "$DIR/term")"; else fail "without ~/.terminfo tput does not know the TERM mwt set"; fi
if [ -x mwt ] && make -s -o mwt install DESTDIR="$DIR/dest" >>"$LOG" 2>&1 &&
	infocmp -D | grep -qx /usr/share/terminfo &&
	TERMINFO_DIRS=$DIR/dest/usr/share/terminfo infocmp -x mwt >/dev/null 2>&1; then
	echo "ok: make install puts the terminfo where ncurses searches"
else
	fail "make install: the terminfo is not in /usr/share/terminfo"
fi
make -s uninstall DESTDIR="$DIR/dest" >>"$LOG" 2>&1
if [ -z "$(find "$DIR/dest" -type f 2>/dev/null)" ]; then echo "ok: make uninstall leaves no files"; else fail "make uninstall leaves: $(find "$DIR/dest" -type f | head -3 | tr '\n' ' ')"; fi

"$MWT" -e sh -c 'exit 7'
echo "info: mwt -e of 'exit 7' ends with code $? (mt passes it on)"
"$MWT" -e /nichtda/prog 2>"$DIR/err"
echo "info: mwt -e of a missing program ends with code $?, stderr '$(head -1 "$DIR/err" | cut -c1-60)'"

grim "$DIR/shot.png" && echo "ok: screenshot"

# ---- last: this one may leave a stuck program behind ----------------------

python3 -c "print(': %060d\n' * 5000 % tuple(range(5000)), end='')" >"$DIR/lines"
T 'sleep 3017' -k Return
wl-copy <"$DIR/lines"
sleep 0.5
K v
sleep 1
C c
sleep 1
if pgrep -f '^sleep 3017$' >/dev/null; then
	fail "Ctrl+C does not get through a paste the program does not read"
	pkill -f '^sleep 3017$'
	pkill -f "^$MWT"
	sleep 0.5
	start_mwt
else
	echo "ok: Ctrl+C gets through a paste the program does not read"
	T 'echo paste-weg' -k Return
	expect "and the rest of the paste is gone" 'paste-weg'
fi

T 'exit' -k Return
for ((i = 0; i < 20; i++)); do
	pgrep -f "^$MWT" >/dev/null || break
	sleep 0.2
done
if pgrep -f "^$MWT" >/dev/null; then fail "mwt stays after the shell exits"; else echo "ok: mwt ends with the shell"; fi
grep -q "ERROR: AddressSanitizer\|runtime error" "$LOG" && { fail "sanitizer"; grep -m1 -A14 "AddressSanitizer\|runtime error" "$LOG"; }
if [ "$FAILS" = 0 ]; then echo "PASS"; else echo "=== $FAILS checks failed ==="; exit 1; fi
