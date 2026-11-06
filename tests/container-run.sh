#!/bin/bash
# Container side of `make evil`: builds from copies of the sources, then runs
# everything and fails if anything did. The layout tests need a vinput that does
# not always say "us", so its copy here gets that.
set -u
fail=0
mkdir -p ~/w/mwc
cp -r /src/mwt ~/w/mwt
cp -r /src/mwc/. ~/w/mwc/
cd ~/w/mwc && rm -rf build mwc .git
sed -i 's/\.layout = "us"/.layout = getenv("VIN_LAYOUT") ? getenv("VIN_LAYOUT") : "us"/' tests/vinput.c
grep -q '<stdlib.h>' tests/vinput.c || sed -i '1i #include <stdlib.h>' tests/vinput.c
make -j"$(nproc)" mwc build/vinput >/dev/null 2>&1 || { echo "mwc did not build"; exit 1; }
export MWC=$HOME/w/mwc/mwc VINPUT=$HOME/w/mwc/build/vinput VIN_LAYOUTS=1
cd ~/w/mwt || exit 1
SAN="-O1 -g -fsanitize=address,undefined"
# From the environment: on the make command line CFLAGS would replace the
# include paths the Makefile appends.
build() {
	make clean >/dev/null
	CFLAGS="$1" LDFLAGS="$2" make -j"$(nproc)" 2>&1 | grep -E 'warning|error'
	[ -x mwt ] || { echo "mwt did not build"; exit 1; }
}
echo "=== make check ==="
make check 2>&1 | tail -1
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
build "-O2 -g" ""
echo "=== make test ==="
tests/run.sh 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
echo "=== evil: timing, without sanitizers ==="
ONLY="flood_ctrl_c flood_rss paste_integrity paste_over_16mb" tests/evil.sh || fail=1
build "$SAN" "-fsanitize=address,undefined"
export ASAN_OPTIONS=detect_leaks=0
echo "=== make test, sanitizers ==="
tests/run.sh 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
echo "=== evil under sway, sanitizers ==="
tests/evil.sh || fail=1
echo "=== evil under mwc, sanitizers ==="
tests/evil-mwc.sh || fail=1
echo "=== terminfo, keys, pixel, mouse, sanitizers ==="
make terminfo 2>&1 | grep -E '^(FAIL|PASS|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
tests/headless.sh tests/keys.py 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
tests/headless.sh tests/pixel.py 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
tests/headless.sh tests/mouse.py 2>&1 | grep -E '^(FAIL|PASS|info|=== )'
[ "${PIPESTATUS[0]}" = 0 ] || fail=1
# Known gaps in character widths are only reported. A crash or a width.sh that
# could not run (its messages start with "width:") fails the run.
echo "=== width, sanitizers (GAPs report only) ==="
tests/width.sh >"$HOME/width.log" 2>&1
grep -E '^(GAP|CRASH|width:|===)' "$HOME/width.log"
grep -qE '^(CRASH|width:)' "$HOME/width.log" && fail=1
[ "$fail" = 0 ] && echo "ALL PASS" || echo "SOMETHING FAILED"
exit "$fail"
