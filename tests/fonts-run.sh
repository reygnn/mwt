#!/bin/bash
# Container side of `make fonts`: tests/fallback.py with a plain build, for the
# timings, and with sanitizers, for what the fallback faces allocate and free.
set -u
fail=0
mkdir -p ~/w/mwc
cp -r /src/mwt ~/w/mwt
cp -r /src/mwc/. ~/w/mwc/
cd ~/w/mwc && rm -rf build mwc .git
grep -q '<stdlib.h>' tests/vinput.c || sed -i '1i #include <stdlib.h>' tests/vinput.c
make -j"$(nproc)" build/vinput >/dev/null 2>&1 || { echo "vinput did not build"; exit 1; }
export VINPUT=$HOME/w/mwc/build/vinput
cd ~/w/mwt || exit 1
build() {
	make clean >/dev/null
	CFLAGS="$1" LDFLAGS="$2" make -j"$(nproc)" 2>&1 | grep -E 'warning|error'
	[ -x mwt ] || { echo "mwt did not build"; exit 1; }
}
echo "=== fonts ==="
mono=$(fc-match -f '%{family}' monospace)
echo "monospace is $mono; $(fc-list | wc -l) faces"
case $mono in
"DejaVu Sans Mono"*) ;;
*) echo "FAIL: monospace should stay DejaVu Sans Mono"; fail=1 ;;
esac
fc-match -f 'emoji goes to %{family} (%{file})\n' 'monospace:charset=1f642'
# fontconfig keeps what it read on its first use for good, as in leaks.py.
printf 'leak:libfontconfig\nleak:libexpat\n' >"$HOME/lsan.supp"
echo "=== font.c out of memory ==="
make -s font_oom 2>&1 | grep -E 'warning|error'
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_oom || fail=1
echo "=== glyph cache limits ==="
make -s font_cache 2>&1 | grep -E 'warning|error'
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_cache || fail=1
echo "=== missing glyphs: one FcFontSort ==="
make -s font_lookup 2>&1 | grep -E 'warning|error'
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_lookup || fail=1
echo "=== styles drawn from regular ==="
make -s font_style 2>&1 | grep -E 'warning|error'
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_style || fail=1
echo "=== two faces of one .ttc ==="
make -s font_ttc 2>&1 | grep -E 'warning|error'
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_ttc || fail=1
echo "=== fallback glyphs: fitted, monospaced first ==="
make -s font_fallback 2>&1 | grep -E 'warning|error'
# D2Coding Bold cut to the hanja Droid Sans Fallback has: a monospaced font that
# adds nothing to the proportional one before it
python3 -I -c '
import logging, sys
logging.disable(logging.WARNING)  # FFTM, which it drops
from fontTools.ttLib import TTFont
from fontTools import subset
d = TTFont("/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf").getBestCmap()
f = TTFont("/usr/share/fonts/truetype/nanum/D2CodingBold-Ver1.3.2-20180524-ligature.ttf")
s = subset.Subsetter()
s.populate(unicodes=[c for c in f.getBestCmap() if 0x4e00 <= c <= 0x9fff and c in d])
s.subset(f)
f.save(sys.argv[1])' "$HOME/hanja-mono.ttf" || fail=1
ASAN_OPTIONS=detect_leaks=1 LSAN_OPTIONS=suppressions=$HOME/lsan.supp:print_suppressions=0 \
	./font_fallback "$HOME/hanja-mono.ttf" || fail=1
build "-O2 -g" ""
echo "=== monospaced fonts without FC_SPACING: no warning ==="
for f in "Noto Sans Mono" "DejaVu Sans Mono" "Noto Mono"; do
	if WAYLAND_DISPLAY=nichtda XDG_RUNTIME_DIR=/tmp ./mwt -f "$f" -e true 2>&1 | grep -q 'not monospaced'; then
		echo "FAIL: '$f' warns that it is not monospaced"
		fail=1
	fi
done
echo "=== fallback ==="
tests/headless.sh tests/fallback.py 2>&1 | grep -E '^(FAIL|PASS|info|ok|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
build "-O1 -g -fsanitize=address,undefined" "-fsanitize=address,undefined"
echo "=== fallback, sanitizers ==="
ASAN_OPTIONS=detect_leaks=0 FALLBACK_NO_TIMING=1 tests/headless.sh tests/fallback.py 2>&1 |
	grep -E '^(FAIL|PASS|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
# leaks.py prints CJK, which here comes from a fallback face, then zooms and
# changes the scale, and each of those loads the fallback faces anew.
echo "=== leaks with fallback faces ==="
tests/headless.sh tests/leaks.py 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOMETHING FAILED"
exit "$fail"
