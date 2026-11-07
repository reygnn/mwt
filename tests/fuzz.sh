#!/bin/bash
# make fuzz: tests/fuzz_vt.c under libFuzzer for FUZZ_TIME seconds (600) on all
# cores. With clang and its fuzzer runtime on the host it runs there, otherwise
# in the container of make evil, which has both. Either way the corpus grows in
# tests/fuzz/corpus (FUZZ_DIR) and a finding is written to tests/fuzz/crash-* (or
# timeout-, oom-, leak-); ./fuzz_vt FILE replays one. Starts from tests/vt100 if
# make replay fetched it.
set -u
cd "$(dirname "$0")/.." || exit 1
FUZZ_DIR=${FUZZ_DIR:-tests/fuzz}
mkdir -p "$FUZZ_DIR/corpus" || exit 1
has_fuzzer() {
	command -v clang >/dev/null &&
		echo 'int LLVMFuzzerTestOneInput(const char *d, unsigned long n) { return 0; }' |
		clang -x c -fsanitize=fuzzer -o /dev/null - >/dev/null 2>&1
}
if ! has_fuzzer; then
	echo "fuzz: no clang with libFuzzer here, running in the container"
	podman build -q -t mwt-test -f tests/Containerfile tests >/dev/null || exit 1
	# The sources read-only, only the fuzz directory writable, so the corpus and
	# the findings stay on the host.
	exec podman run --rm --network=none --userns=keep-id \
		-e FUZZ_TIME="${FUZZ_TIME:-600}" -e FUZZ_DIR=/fuzz \
		-v "$PWD:/src/mwt:ro" -v "$(realpath "$FUZZ_DIR"):/fuzz:rw" mwt-test \
		bash -c 'cp -r /src/mwt "$HOME/w" && exec "$HOME/w/tests/fuzz.sh"'
fi
make -s fuzz_vt || exit 1
seeds=()
[ -d tests/vt100/videos ] && seeds=(tests/vt100/videos)
exec ./fuzz_vt -fork="$(nproc)" -max_total_time="${FUZZ_TIME:-600}" -max_len=4098 \
	-dict=tests/fuzz_vt.dict -artifact_prefix="$FUZZ_DIR/" "$FUZZ_DIR/corpus" "${seeds[@]}"
