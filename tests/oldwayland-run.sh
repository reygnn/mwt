#!/bin/bash
# Container side of `make oldwayland`: evil.sh's cases that flood the compositor
# with requests, against libwayland 1.22. A plain build: what fails here is the
# connection, not memory, and make evil has the sanitizers.
set -u
mkdir -p ~/w/mwc
cp -r /src/mwt ~/w/mwt
cp -r /src/mwc/. ~/w/mwc/
# vinput alone: mwc's Makefile wants all of mwc's dependencies first.
cd ~/w/mwc && rm -rf build mwc .git && mkdir build || exit 1
for x in wlr-virtual-pointer-unstable-v1 virtual-keyboard-unstable-v1; do
	wayland-scanner client-header protocols/$x.xml build/$x-client.h &&
	wayland-scanner private-code protocols/$x.xml build/$x-code.c || exit 1
done
cc -O2 -g -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild -o build/vinput tests/vinput.c \
	build/wlr-virtual-pointer-unstable-v1-code.c build/virtual-keyboard-unstable-v1-code.c \
	-lwayland-client -lm -lxkbcommon || { echo "vinput did not build"; exit 1; }
export VINPUT=$HOME/w/mwc/build/vinput
cd ~/w/mwt || exit 1
make clean >/dev/null
CFLAGS="-O2 -g" make -j"$(nproc)" mwt 2>&1 | grep -E 'warning|error'
[ -x mwt ] || { echo "mwt did not build"; exit 1; }
echo "=== libwayland $(pkg-config --modversion wayland-client), $(sway --version) ==="
ONLY="title_flood title_flood_stalled" tests/evil.sh
