# mwt – minimal Wayland terminal

A small terminal emulator for Wayland, written in C. mwt is a standalone xdg-shell client and needs neither X11 nor Xwayland. It runs in mwc and in any other compositor.

## Building

Dependencies: wayland-client, wayland-cursor, wayland-protocols, wayland-scanner, xkbcommon, fontconfig, freetype2.

```sh
make            # builds ./mwt
make check      # tests for the VT core (headless, with ASan/UBSan), including a fuzzer and the kept fuzz findings in tests/fuzz-regress
make replay     # about 290 old VT100 animations and ANSI files through the VT core (fetches them from GitHub on the first run)
make fuzz       # coverage-guided fuzzing of the VT core with libFuzzer, 10 minutes (FUZZ_TIME=seconds), with clang on the host or in the Podman container
make terminfo   # each capability in mwt.info against the VT core (needs python3 and ncurses)
make keys       # what mwt sends for keys and the mouse, against mwt.info (under headless sway, like make test)
make pixel      # what mwt draws, by screenshot (under headless sway, like make test)
make mouse      # what mwt reports in each mouse mode (under headless sway, like make test)
make keyboard   # dead keys, Compose, and key repeat (under headless sway, like make test)
make test       # black-box tests under headless sway (needs sway, wtype, wl-clipboard, grim, and vinput from mwc)
make width      # where mwt and bash, vim, or tmux count character widths differently (also needs vim and tmux)
make esctest    # esctest's escape sequence tests in mwt under headless sway, a report (fetches esctest on the first run)
make evil       # all tests plus adversarial cases in a Podman container, also with sanitizers
make fonts      # glyphs from fallback faces (CJK, emoji, many scripts), in a container with more fonts
make oldwayland # the title floods against libwayland 1.22 (Ubuntu 24.04), in a container
make profile    # where mwt spends its time, under load and at 60 Hz, in the same container (measures only)
sudo make install   # installs mwt to /usr/local and the terminfo entry to /usr/share/terminfo
sudo make uninstall # removes both again
```

On its first run, `make replay` downloads its files to `tests/vt100`. They are not in the repository because their license is unclear. The `make evil` container has no network and skips the replay if they are missing: run `tests/replay.sh --fetch` once on the host to get them. The same goes for `make esctest` and `tests/esctest`: esctest is GPL, so it is fetched, not included, and `tests/esctest.sh --fetch` gets it.

Without installing, `tic -x mwt.info` is enough. It puts the entry in `~/.terminfo`. If the entry is missing, mwt sets `TERM=xterm-256color` and prints a warning.

## Usage

```
mwt [-v] [-f font] [-a app-id] [-T title] [-g COLSxROWS] [-e] [command [args...]]
```

* `-v` prints the version, set in `version.h`, which XTVERSION also reports.
* `-f "JetBrains Mono:size=12"` takes a fontconfig pattern.
* `-a` sets the app_id, which defaults to `mwt`. The mwmrc uses it to assign colors and position.
* `-g` takes 1x1 to 4096x2048, as long as the window buffer stays under 2 GiB. With a 9x18 pixel font, that goes up to about 1800x1800; beyond that, mwt refuses with exit code 2. The grid never gets larger than 4096x2048, even in the largest window.
* Without a command, mwt starts `$SHELL`.

Everything else, meaning colors, font, padding, and scrollback, is in `config.h` and takes effect when you rebuild.

### mwtrc

The colors can also be set without rebuilding, in `$XDG_CONFIG_HOME/mwt/mwtrc` (or `~/.config/mwt/mwtrc`). mwt reads it at startup, over what `config.h` says. The syntax is that of mwc's mwmrc:

```
! IRIS Indigo winterm
Mwt*foreground:            #e5e5e5
Mwt*background:            #000040
! only for mwt -a indigo
Mwt*indigo*foreground:     #dcdcdc
```

* Resources: `foreground`, `background`, `cursorColor`, `selectionForeground`, `selectionBackground`, and `color0` to `color15` for the 16 palette colors, named as in xterm. Without `cursorColor`, the cursor takes the foreground. The 240 colors above them stay fixed.
* Colors are `#rrggbb`, `#rgb` or `rgb:rr/gg/bb`. There are no color names.
* A line with an app_id (`-a`) wins over a global one, and later lines win over earlier ones. Names and app_ids are compared ignoring case.
* Lines starting with `!` are comments. There are no comments after a value.
* A bad line is skipped, and stderr names the file and line. A missing file is not an error.
* On a light background, set `color7` and `color15` as well: those of `config.h` are hard to read there.
* `doc/mwtrc` is an example with four complete color sets, all 21 colors each: `indigo3`, after the SGI IRIS Indigo winterm; `dracula`, a softened Dracula; `paper`, dark on cream for daylight; and `amber`, after amber phosphor terminals. Try them with `mwt -a indigo3`, for example.

## Controls

| Input | Effect |
|---|---|
| Drag with the left mouse button | Select; copied to the primary selection on release |
| Double-/triple-click | Select word/line |
| Right-click | Menu: Copy, Paste, Select All. Arrow keys and Return also work, Escape closes it |
| Shift+left-click | Extend the selection |
| Middle-click, Shift+Insert | Paste the primary selection |
| Ctrl+Shift+A | Select all: scrollback and screen, only the screen in the alternate screen |
| Ctrl+Shift+C / V | Copy to / paste from the clipboard |
| Shift+PgUp / PgDn | Scrollback (in the alternate screen they go to the program) |
| Mouse wheel | Scrollback |
| Shift+mouse wheel | Scrollback by page |
| Shift+Home / End | To the start / end of the scrollback (not when a program uses the alternate screen or the mouse) |
| Ctrl+Shift+Plus / Minus / 0 | Font larger / smaller / reset |
| Shift + mouse | Selection and menu even when a program has taken over the mouse |

## Structure

| File | Contents |
|---|---|
| `vt.c`, `vt.h` | Parser based on Paul Williams' DEC state machine, grid, scrollback. No Wayland code, so it can be tested headless. |
| `font.c` | fontconfig/FreeType, four styles, fallback fonts for missing glyphs, glyph cache |
| `mwt.c` | Wayland (xdg-shell, xdg-decoration, wl_shm, wl_data_device, primary selection), keyboard through xkbcommon including Compose/dead keys, mouse, pty, event loop |
| `mwt.info` | terminfo: declares only what `vt.c` can really do |
| `rc.c` | the mwtrc: colors at startup, no Wayland code |
| `doc/mwtrc` | an example mwtrc |
| `version.h` | the version, for `-v` and the XTVERSION reply |
| `test_vt.c` | Tests for the VT core |
| `test_rc.c` | Tests for reading the mwtrc |
| `tests/replay.c` | Plays back recorded terminal output: in one piece, byte by byte, in random chunks, and with resizes. The first three must end the same, and the grid must be consistent after every write. `make replay REPORT=1` lists, per file, what mwt cannot do from it. |
| `tests/fuzz_vt.c` | libFuzzer harness for the VT core: one write against chunks cut by the input itself, must end the same; then resizes, scrolling through the history and a reset, with the grid checked after each step. Starts from the `make replay` files if they are there. Built without clang (`-DFUZZ_STANDALONE`) it is `fuzz_regress`, which `make check` runs over `tests/fuzz-regress`: what `make fuzz` found, kept after the fix as a regression test (how: see the head of `tests/fuzz_vt.c`). |
| `tests/` | Black-box tests and adversarial cases, taken over from the mt project. `MWC_DIR` names the mwc source tree and defaults to `../../mwc`. |

Only changed rows are drawn. With double buffering, the rows that differ from the most recently shown buffer are copied first. Output is tied to frame callbacks.

## Supported

* VT100/VT220 core: cursor movement, ED/EL/ECH, ICH/DCH, IL/DL, SU/SD, scroll regions, origin mode, tabs, REP, DEC special graphics (G0/G1)
* SGR: bold, dim, italic, underline, reverse, invisible, strike; 16, 256, and truecolor (`;` and `:` forms)
* Alternate screen (47/1047/1049), bracketed paste, mouse (1000/1002/1003 + SGR 1006), focus events, DECSCUSR (cursor shape)
* Replies to DA, DA2, DSR/CPR, DECRQM, XTVERSION, window size (CSI 18 t). DECRQM reports the modes mwt knows of but does not have as permanently reset (4)
* Keypad application mode (ESC = / ESC >, DECNKM `?66`) is kept and reported by DECRQM, but the keypad sends what it sends without it, as in xterm with its default mode 1035: see CAVEATS
* DECRQCRA (checksum of a rectangle, as xterm computes it), only with `allow_checksum` in `config.h`: like xterm, off by default, since it lets any program read the screen. `make esctest` builds `mwt-esctest` with it on.
* OSC 0/2 (window title); OSC 4, 10, 11 and 12 queries are answered with the colors as drawn, from `config.h` and the mwtrc (programs use them to tell a dark background from a light one), setting colors is ignored
* Wide characters (CJK, emoji as outlines, if a fallback font has them)
* HiDPI through `wl_surface.preferred_buffer_scale` (integer scaling)

## Deliberately left out

Tektronix, VT52, Sixel/ReGIS, DCS queries, reflow on resize, ligatures, bidi, combining characters (they are discarded), color emoji bitmaps, subpixel AA, fractional scaling, drag and drop, OSC 52.

Of the VT420/xterm features esctest checks, these are left out as well, so most of what `make esctest` reports fails by design: left and right margins (DECLRMM/DECSLRM), rectangle operations (DECCRA, DECFRA, DECERA), inserting and deleting columns (DECIC, DECDC, DECBI, DECFI), protected areas and selective erase (DECSCA, SPA/EPA, DECSED, DECSEL), window operations (XTWINOPS beyond CSI 18 t), setting colors (OSC 4/10–19/104), reverse wraparound (`?45`), 132 columns (DECCOLM), reverse screen (DECSCNM), double-size lines (`ESC # 3`–`6`), national character sets and G2/G3, XTSAVE/XTRESTORE, DECSCL, and the DECDSR reports for printer, keyboard and locator.

## Notes on mwc

* Frames come through xdg-decoration (server-side).
* Size increments: xdg-shell has no size step like `WM_NORMAL_HINTS`. So mwt snaps a floating window to whole cells itself. When maximized, fullscreen, or tiled, mwt takes the given size and fills the rest with the background color. This is why mwc's resize outline and size display show pixels instead of columns and rows.
* With ssh to hosts without the mwt terminfo entry: use `TERM=xterm-256color ssh …` or install the entry there with `infocmp -x mwt | ssh host tic -x -`.
