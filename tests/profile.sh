#!/bin/bash
# Host side of `make profile`: tests/profile-run.sh in the image `make evil` uses,
# with the sources read-only and no network, so a running session is not touched.
# PROFILE_WORK picks loads, PROFILE_GRID and PROFILE_RES the window and the output.
set -u
cd "$(dirname "$0")/.." || exit 1
podman build -q -t mwt-test -f tests/Containerfile tests >/dev/null || exit 1
exec podman run --rm --network=none --tmpfs /tmp/.X11-unix:mode=1777 \
	-e PROFILE_WORK="${PROFILE_WORK:-}" -e PROFILE_GRID="${PROFILE_GRID:-200x55}" \
	-e PROFILE_RES="${PROFILE_RES:-1920x1080}" \
	-v "$PWD:/src/mwt:ro" mwt-test bash /src/mwt/tests/profile-run.sh
