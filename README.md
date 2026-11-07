# mwt – minimal Wayland terminal

A small terminal emulator for Wayland, written in C. mwt is a standalone xdg-shell client and needs neither X11 nor Xwayland. It runs in mwc and in any other compositor.

## Building

Dependencies: wayland-client, wayland-cursor, wayland-protocols, wayland-scanner, xkbcommon, fontconfig, freetype2.

```sh
make            # builds ./mwt
make check      # tests for the VT core (headless, with ASan/UBSan), including a fuzzer
make replay     # about 290 old VT100 animations and ANSI files through the VT core (fetches them from GitHub on the first run)
make terminfo   # each capability in mwt.info against the VT core (needs python3 and ncurses)
make keys       # what mwt sends for keys and the mouse, against mwt.info (under headless sway, like make test)
make pixel      # what mwt draws, by screenshot (under headless sway, like make test)
make mouse      # what mwt reports in each mouse mode (under headless sway, like make test)
make keyboard   # dead keys, Compose, and key repeat (under headless sway, like make test)
make test       # black-box tests under headless sway (needs sway, wtype, wl-clipboard, grim, and vinput from mwc)
make width      # where mwt and bash, vim, or tmux count character widths differently (also needs vim and tmux)
make evil       # all tests plus adversarial cases in a Podman container, also with sanitizers
make profile    # where mwt spends its time, under load and at 60 Hz, in the same container (measures only)
sudo make install   # installs mwt to /usr/local and the terminfo entry to /usr/share/terminfo
sudo make uninstall # removes both again
```

On its first run, `make replay` downloads its files to `tests/vt100`. They are not in the repository because their license is unclear. The `make evil` container has no network and skips the replay if they are missing: run `tests/replay.sh --fetch` once on the host to get them.

Without installing, `tic -x mwt.info` is enough. It puts the entry in `~/.terminfo`. If the entry is missing, mwt sets `TERM=xterm-256color` and prints a warning.

## Usage

```
mwt [-f font] [-a app-id] [-T title] [-g COLSxROWS] [-e] [command [args...]]
```

* `-f "JetBrains Mono:size=12"` takes a fontconfig pattern.
* `-a` sets the app_id, which defaults to `mwt`. The mwmrc uses it to assign colors and position.
* `-g` takes 1x1 to 4096x2048, as long as the window buffer stays under 2 GiB. With a 9x18 pixel font, that goes up to about 1800x1800; beyond that, mwt refuses with exit code 2. The grid never gets larger than 4096x2048, even in the largest window.
* Without a command, mwt starts `$SHELL`.

Everything else, meaning colors, font, padding, and scrollback, is in `config.h` and takes effect when you rebuild.

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
| `test_vt.c` | Tests for the VT core |
| `tests/replay.c` | Plays back recorded terminal output: in one piece, byte by byte, in random chunks, and with resizes. The first three must end the same, and the grid must be consistent after every write. `make replay REPORT=1` lists, per file, what mwt cannot do from it. |
| `tests/` | Black-box tests and adversarial cases, taken over from the mt project. `MWC_DIR` names the mwc source tree and defaults to `../../mwc`. |

Only changed rows are drawn. With double buffering, the rows that differ from the most recently shown buffer are copied first. Output is tied to frame callbacks.

## Supported

* VT100/VT220 core: cursor movement, ED/EL/ECH, ICH/DCH, IL/DL, SU/SD, scroll regions, origin mode, tabs, REP, DEC special graphics (G0/G1)
* SGR: bold, dim, italic, underline, reverse, invisible, strike; 16, 256, and truecolor (`;` and `:` forms)
* Alternate screen (47/1047/1049), bracketed paste, mouse (1000/1002/1003 + SGR 1006), focus events, DECSCUSR (cursor shape)
* Replies to DA, DA2, DSR/CPR, DECRQM, XTVERSION, window size (CSI 18 t)
* OSC 0/2 (window title)
* Wide characters (CJK, emoji as outlines, if a fallback font has them)
* HiDPI through `wl_surface.preferred_buffer_scale` (integer scaling)

## Deliberately left out

Tektronix, VT52, Sixel/ReGIS, DCS queries, reflow on resize, ligatures, bidi, combining characters (they are discarded), color emoji bitmaps, subpixel AA, fractional scaling, drag and drop, OSC 52.

## Notes on mwc

* Frames come through xdg-decoration (server-side).
* Size increments: xdg-shell has no size step like `WM_NORMAL_HINTS`. So mwt snaps a floating window to whole cells itself. When maximized, fullscreen, or tiled, mwt takes the given size and fills the rest with the background color. This is why mwc's resize outline and size display show pixels instead of columns and rows.
* With ssh to hosts without the mwt terminfo entry: use `TERM=xterm-256color ssh …` or install the entry there with `infocmp -x mwt | ssh host tic -x -`.
