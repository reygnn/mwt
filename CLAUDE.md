# mwt

Project rules. The rules for all C projects, mwc as their reference, are in the global
CLAUDE.md.


## Build and test

The host has no sway, grim, wtype or clang; what needs them runs in Podman. Tests that
use mwc's `vinput` need `MWC_DIR`, which defaults to `../../mwc`:
`MWC_DIR=~/data/C-Projects/mwc make evil`.

| Target | |
|---|---|
| `make` | mwt |
| `make check` | `test_vt`, `test_rc`, `test_input`, `test_paste`, `test_sel`, `test_shell`, `test_util` and the kept fuzz findings in `tests/fuzz-regress`, under ASan and UBSan, and mwt's command line (`tests/args.sh`), headless |
| `make replay` | about 290 recorded VT100 animations and ANSI files through the VT core; `REPORT=1` lists what mwt lacks |
| `make fuzz` | libFuzzer on the VT core, `FUZZ_TIME` seconds (600); clang on the host or the `make evil` container |
| `make terminfo` | each capability of `mwt.info` against the VT core; python3 and ncurses |
| `make keys` | what mwt sends for keys and the mouse, against `mwt.info`, under headless sway |
| `make bell` | BEL marks the window urgent without the focus, once, under headless sway |
| `make wayland` | a hidden window's configure, the min size, long `-T` and `-a`, from mwt's Wayland log, under headless sway |
| `make pixel` | what mwt draws, by screenshot, under headless sway |
| `make mouse` | the mouse modes, under headless sway |
| `make keyboard` | dead keys, Compose and key repeat, under headless sway |
| `make test` | black-box tests under headless sway (`tests/run.sh`) |
| `make width` | where mwt and bash, vim or tmux count widths differently |
| `make esctest` | esctest in `mwt-esctest`, a report; most failures are by design (CAVEATS.md) |
| `make evil` | all tests plus adversarial cases in Podman, also with sanitizers, about 20 minutes |
| `make fonts` | fallback faces, styles and a `.ttc` (`font_oom`, `font_cache`, `font_lookup`, `font_style`, `font_ttc`) in an image with more fonts |
| `make oldwayland` | the title floods against libwayland 1.22 (Ubuntu 24.04) in Podman |
| `make profile` | where mwt spends its time, in the `make evil` container; measures only |
| `make install`, `make uninstall` | the user's |

CI (`.github/workflows/check.yml`) runs a `-Werror` build, `make check`, `make terminfo`
and `make replay`.

`tests/vt100` and `tests/esctest` are fetched, not kept: the first has no clear license,
esctest is GPL. The containers have no network, so fetch on the host first with
`tests/replay.sh --fetch` and `tests/esctest.sh --fetch`.

`perf` is blocked on the host (`kernel.perf_event_paranoid=4`); `make profile` with
`tests/profsampler.c` stands in. Leave the sysctl alone.


## Structure

| File | |
|---|---|
| `vt.c`, `vt.h` | parser after Paul Williams' DEC state machine, grid, scrollback; no Wayland, so testable headless |
| `font.c` | fontconfig and FreeType, four styles, fallback faces, glyph cache |
| `mwt.c` | Wayland (xdg-shell, xdg-decoration, viewporter, xdg-activation, wl_shm, data device, primary selection), keyboard and pointer events, pty, event loop |
| `input.c` | what keys and the mouse send: xkbcommon with Compose, shortcuts, mouse reports; no Wayland |
| `paste.c` | what a paste becomes: UTF-8 checks, decoding, the control filter, the brackets; the clipboard's types and Latin-1 |
| `sel.c` | the selection over the VT core's lines: words, lines, clicks, drags, its text |
| `shell.c` | the shell on its pty: what waits to be written, the paste dropped for a signal; spawn, reap, hang up |
| `util.c` | `-g`, the context menu's keys, the first size within the compositor's bounds, cutting `-T` and `-a` |
| `rc.c` | the mwtrc, the minimum contrast and the cursor's color on a cell; no Wayland |
| `mwt.info` | terminfo; declares only what `vt.c` really does |
| `config.h`, `version.h` | defaults; the version for `-v` and XTVERSION |
| `test_vt.c`, `test_rc.c`, `test_input.c`, `test_paste.c`, `test_sel.c`, `test_shell.c`, `test_util.c` | unit tests; `test_vt`, `test_paste` and `test_sel` fail each allocation once through `--wrap`, `test_input` compiles keymaps from layout names, `test_shell` runs real ptys, pipes and processes |
| `tests/replay.c` | plays recordings whole, byte by byte, in chunks and with resizes; the first three must end the same |
| `tests/fuzz_vt.c` | libFuzzer harness; built with `-DFUZZ_STANDALONE` it is `fuzz_regress`. How to keep a finding: its head |
| `tests/` | black-box and adversarial tests, from the mt project; `tests/mwtlib.py` for the Python ones |

Only changed rows are drawn. With double buffering, rows that differ from the buffer
shown last are copied first; output follows frame callbacks. A build with
`-DMWT_CHECK_REDRAW` compares each frame with a full redraw; `make evil` runs `make
pixel` with it.


## Rules

- `mwt.info` promises nothing `vt.c` doesn't do; `make terminfo` and `make keys` check
  it.
- A fuzz finding stays in `tests/fuzz-regress` after its fix.
