#!/bin/bash
# Host side of `make oldwayland`: builds the image (tests/Containerfile.2404) and
# runs tests/oldwayland-run.sh in it, with the sources read-only and no network.
# MWC_DIR is mwc's source tree, for vinput.
set -u
cd "$(dirname "$0")/.." || exit 1
MWC_DIR=$(readlink -f "${MWC_DIR:-../../mwc}")
[ -f "$MWC_DIR/tests/vinput.c" ] || { echo "no mwc sources in $MWC_DIR (MWC_DIR)"; exit 1; }
podman build -q -t mwt-test-2404 -f tests/Containerfile.2404 tests >/dev/null || exit 1
exec podman run --rm --network=none --tmpfs /tmp/.X11-unix:mode=1777 \
	-v "$PWD:/src/mwt:ro" -v "$MWC_DIR:/src/mwc:ro" mwt-test-2404 bash /src/mwt/tests/oldwayland-run.sh "$@"
