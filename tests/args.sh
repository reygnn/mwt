#!/bin/sh
# What mwt's command line does before it needs a compositor: -v, -h, bad options
# and geometries, a font size out of range, a font that is not monospaced, and
# no display to connect to. For make check, so CI sees it. The checks that need
# a font are skipped where fontconfig finds none.
cd "$(dirname "$0")/.." || exit 1
MWT=${MWT:-./mwt}
[ -x "$MWT" ] || { echo "args: $MWT not built"; exit 2; }
DIR=$(mktemp -d /tmp/mwt-args.XXXXXX) || exit 2
trap 'rm -rf "$DIR"' EXIT
# no compositor: a runtime directory without a socket in it
export XDG_RUNTIME_DIR=$DIR WAYLAND_DISPLAY=nichtda
unset DISPLAY
fails=0 n=0

# run <exit code> <stderr pattern or -> <args...>
run() {
	want=$1 pat=$2
	shift 2
	n=$((n + 1))
	"$MWT" "$@" >"$DIR/out" 2>"$DIR/err" </dev/null
	rc=$?
	if [ "$rc" != "$want" ]; then
		echo "args: FAIL mwt $*: exit $rc, expected $want; $(head -1 "$DIR/err")"
		fails=$((fails + 1))
	elif [ "$pat" != - ] && ! grep -q -- "$pat" "$DIR/err"; then
		echo "args: FAIL mwt $*: stderr '$(head -1 "$DIR/err")', expected '$pat'"
		fails=$((fails + 1))
	fi
}

version=$(sed -n 's/^#define MWT_VERSION "\(.*\)"$/\1/p' version.h)
n=$((n + 1))
out=$("$MWT" -v 2>&1)
rc=$?
if [ "$rc" != 0 ] || [ "$out" != "mwt $version" ]; then
	echo "args: FAIL mwt -v: '$out', exit $rc, expected 'mwt $version', exit 0"
	fails=$((fails + 1))
fi
run 2 '^usage: mwt' -h
run 2 '^usage: mwt' -x
run 2 '^usage: mwt' -g
for g in 0x0 10x x 10 -1x-1 80x0 99999999999999999999x1 1x99999999999999999999 \
	4097x1 1x2049 '80x24 ' 80X24; do
	run 2 - -g "$g"
done
if fc-match -f '%{file}' monospace 2>/dev/null | grep -q .; then
	run 2 'needs a buffer over 2 GiB' -g 4096x2048
	run 1 'out of range' -f 'monospace:size=100000'
	run 1 'cannot connect to Wayland display' -g 80x24
	run 1 'cannot connect to Wayland display' -g 1x1 -e true
	# a family fontconfig does not know: a warning where it gives a proportional one
	case $(fc-match -f '%{spacing}' NoSuchFamilyMwt 2>/dev/null) in
	90 | 100 | 110) ;;
	*) run 1 'not monospaced' -f NoSuchFamilyMwt ;;
	esac
	n=$((n + 1))
	"$MWT" -f monospace -g 80x24 >"$DIR/out" 2>"$DIR/err" </dev/null
	if grep -q 'not monospaced' "$DIR/err"; then
		echo "args: FAIL mwt -f monospace warns: $(head -1 "$DIR/err")"
		fails=$((fails + 1))
	fi
else
	echo "args: no font, the checks that need one are skipped"
fi
[ "$fails" = 0 ] && echo "args: all $n passed"
[ "$fails" = 0 ]
