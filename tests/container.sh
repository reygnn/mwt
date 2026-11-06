#!/bin/bash
# Host side of `make evil`: builds the image and runs tests/container-run.sh in
# it, with the sources read-only and no network. MWC_DIR is mwc's source tree.
set -u
cd "$(dirname "$0")/.." || exit 1
MWC_DIR=$(readlink -f "${MWC_DIR:-../../mwc}")
[ -f "$MWC_DIR/tests/vinput.c" ] || { echo "no mwc sources in $MWC_DIR (MWC_DIR)"; exit 1; }
podman build -q -t mwt-test -f tests/Containerfile tests >/dev/null || exit 1
exec podman run --rm --network=none --tmpfs /tmp/.X11-unix:mode=1777 \
	-v "$PWD:/src/mwt:ro" -v "$MWC_DIR:/src/mwc:ro" mwt-test bash /src/mwt/tests/container-run.sh "$@"
