#!/bin/bash
# Plays recorded terminal output through the VT core (tests/replay.c): VT100
# animations from textfiles.com, grox.net and Usenet, and ANSI art, about 290
# files and 19 MB from Eli the Bearded's vt100-slowcat collection. They are not
# in this repository, their licenses are unclear; the first run clones them,
# pinned to one commit, into tests/vt100 (VT_DIR). Without network, run
# `tests/replay.sh --fetch` once beforehand.
#
#   tests/replay.sh            check all files, print failures and a summary
#   tests/replay.sh -r         also list per file what mwt does not support
#   tests/replay.sh f.vt ...   only these files
#   tests/replay.sh --fetch    only fetch the files
set -u
cd "$(dirname "$0")/.." || exit 1
VT_DIR=${VT_DIR:-tests/vt100}
REPO=https://github.com/Eli-the-Bearded/vt100-slowcat
COMMIT=182d416046991981ec775346c67f24e0a54a31cf

fetch() {
	[ -d "$VT_DIR/videos" ] && return 0
	echo "replay: fetching $REPO at ${COMMIT:0:12} into $VT_DIR"
	rm -rf "$VT_DIR.tmp"
	git init -q "$VT_DIR.tmp" &&
	git -C "$VT_DIR.tmp" fetch -q --depth 1 "$REPO" "$COMMIT" &&
	git -C "$VT_DIR.tmp" -c advice.detachedHead=false checkout -q FETCH_HEAD &&
	mv "$VT_DIR.tmp" "$VT_DIR" || { echo "replay: fetch failed"; rm -rf "$VT_DIR.tmp"; return 1; }
}

if [ "${1:-}" = --fetch ]; then
	fetch
	exit
fi
[ -x ./test_replay ] || { echo "replay: ./test_replay not built (make test_replay)"; exit 2; }

opts=(-q)
[ "${1:-}" = -r ] && { opts+=(-r); shift; }
if [ $# -gt 0 ]; then
	exec ./test_replay "${opts[@]}" "$@"
fi
fetch || exit 2
# Everything except the descriptions, the fetch lists and the README notes.
mapfile -d '' files < <(find "$VT_DIR/videos" -type f \
	! -name '*.desc' ! -name '*.fetch' ! -name '*.txt' -print0 | sort -z)
[ ${#files[@]} -gt 0 ] || { echo "replay: no files in $VT_DIR/videos"; exit 2; }
exec ./test_replay "${opts[@]}" "${files[@]}"
