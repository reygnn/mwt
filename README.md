# mwt

mwt is a small terminal emulator for Wayland, written in C. It is a plain xdg-shell
client and needs neither X11 nor Xwayland, so it runs in mwc and in any other
compositor.


## Build

Dependencies: wayland-client, wayland-cursor, wayland-protocols, wayland-scanner,
xkbcommon, fontconfig, freetype2.

    make
    make test
    sudo make install

`make test` runs black-box tests under a headless sway and needs sway, wtype,
wl-clipboard, grim and `vinput` from mwc. `make install` puts `mwt` in `/usr/local/bin`
and the terminfo entry in `/usr/share/terminfo`, where ncurses looks; `make uninstall`
removes both.

Without installing, `tic -x mwt.info` puts the entry in `~/.terminfo`. If the entry is
missing, mwt sets `TERM=xterm-256color` and prints a warning. On ssh hosts without it,
use `TERM=xterm-256color ssh host` or `infocmp -x mwt | ssh host tic -x -`.


## Usage

    mwt [-v] [-h] [-f font] [-a app-id] [-T title] [-g COLSxROWS] [-e] [command [args...]]

* `-v` prints the version, set in `version.h`, and the commit built, as
  `0.3.5+DATE.TIME.HASH` with its commit time in UTC and `.dirty` for changes not
  committed. XTVERSION reports the same.
* `-h` prints the usage and exits with code 2.
* `-f "JetBrains Mono:size=12"` takes a fontconfig pattern.
* `-a` sets the app_id, default `mwt`. mwc's mwmrc and the mwtrc match on it.
* `-T` sets the window title, default `mwt`, until a program sets its own.
* `-e` ends the options; what follows is the command.
* `-g` takes 1x1 to 4096x2048, as long as the window buffer stays under 2 GiB;
  beyond that mwt exits with code 2.
* Without a command, mwt starts `$SHELL`.

Colors, font, padding and scrollback are set in `config.h` and take effect on a
rebuild. The colors and what opens a URL can also be set in the mwtrc.


## mwtrc

`$XDG_CONFIG_HOME/mwt/mwtrc`, or `~/.config/mwt/mwtrc`, is read at startup over what
`config.h` says. The syntax is that of mwc's mwmrc:

    ! IRIS Indigo winterm
    Mwt*foreground:            #e5e5e5
    Mwt*background:            #000040
    ! only for mwt -a indigo
    Mwt*indigo*foreground:     #dcdcdc

| Resource | Meaning |
|---|---|
| `foreground`, `background` | text and background |
| `cursorColor` | the cursor; default the foreground |
| `selectionForeground`, `selectionBackground` | selected text |
| `color0` to `color15` | the 16 palette colors, as in xterm; the 240 above stay fixed |
| `minimumContrast` | WCAG ratio 1 to 21, default 3; `0` turns it off |
| `urlLauncher` | what opens a URL, default `xdg-open`; its words, then the URL |

Text whose contrast to its background is below `minimumContrast` is drawn in the
foreground or the background, whichever stands out more. That keeps htop's header
readable in a light theme and mc's panels in a dark one. The palette itself stays as
set, and text in the color of its background stays hidden. Blocks and shades
(U+2580 to U+259F) keep their colors: in pictures drawn with ▀ and ▄ both are pixels.

`urlLauncher` is split at blanks, without a shell or quotes, into at most 30 words
and 255 bytes:
`firefox --new-tab` starts `firefox --new-tab URL`.

Colors are `#rrggbb`, `#rgb` or `rgb:rr/gg/bb`, without names. A line with an app_id
wins over a global one, and later lines win over earlier ones; names and app_ids
ignore case. Lines starting with `!` are comments; there are none after a value. A bad
line is skipped and named on stderr; a missing file is no error. On a light
background, set `color7` and `color15` too.

`docs/mwtrc` has four complete color sets: `indigo3` after the SGI IRIS Indigo winterm,
`dracula`, `paper` (dark on cream) and `amber`. Copy it to `~/.config/mwt/mwtrc` and try
them with `mwt -a indigo3`.


## Controls

| Input | Effect |
|---|---|
| Drag with button 1 | select; copied to the primary selection on release |
| Double or triple click | select a word or a line |
| Shift+click | extend the selection |
| Ctrl+click | open the URL under the pointer; with Ctrl held it is underlined |
| Button 3 | menu: Copy, Paste, Select all, and Open link on a URL; arrows, Return and Escape work in it |
| Button 2, Shift+Insert | paste the primary selection |
| Ctrl+Shift+A | select all: scrollback and screen, in the alternate screen only the screen |
| Ctrl+Shift+C, Ctrl+Shift+V | copy to and paste from the clipboard |
| Shift+PgUp, Shift+PgDn | scrollback; in the alternate screen they go to the program |
| Wheel, Shift+wheel | scrollback by lines or by pages; in the alternate screen three arrow keys a notch, unless a program has the mouse |
| Shift+Home, Shift+End | start or end of the scrollback, unless a program has the alternate screen or the mouse |
| Ctrl+Shift+Plus, Minus, 0 | font larger, smaller, reset |
| Shift with the mouse | selection and menu while a program has the mouse |


## Supported

* VT100 and VT220 core: cursor movement, ED, EL, ECH, ICH, DCH, IL, DL, SU, SD, scroll
  regions, origin mode, tabs, REP, DEC special graphics in G0 and G1
* SGR: bold, dim, italic, underline, reverse, invisible, strike; 16, 256 and truecolor
* Alternate screen (47, 1047, 1049), bracketed paste, mouse (1000, 1002, 1003, SGR
  1006), focus events, DECSCUSR
* Replies to DA, DA2, DSR, CPR, DECRQM, XTVERSION and CSI 18 t
* OSC 0 and 2 (title); OSC 4, 10, 11 and 12 queries, answered with the colors as
  configured in `config.h` and the mwtrc
* DECRQCRA only with `allow_checksum` in `config.h`, off by default as in xterm
* Wide characters, and emoji as outlines if a fallback font has them
* Reflow: a window of another width wraps the lines of the screen and of the
  scrollback anew, and a taller one takes lines back from the scrollback
* Box drawing and block elements (U+2500 to U+259F) and the DEC scan lines (U+23BA to
  U+23BD) drawn to fill the cell, so lines join between rows whatever the font
* The launcher's xdg-activation token, taken for the window and kept from the shell
* HiDPI through `wl_surface.preferred_buffer_scale`, integer scales only
* URLs (http, https, ftp, file, mailto) found in the text, opened with an
  xdg-activation token, so the browser may come to the front; an opener that is not
  there shows a toast

COMPAT.md goes through the VT100's sequences, and those after it, one by one.


## Deliberately left out

Tektronix, VT52, Sixel and ReGIS, DCS queries, OSC 8 hyperlinks, ligatures, bidi,
combining characters, color emoji bitmaps, subpixel antialiasing, fractional scaling,
drag and drop, OSC 52, and most VT420 and xterm extensions that esctest checks. The
list and other limits are in CAVEATS.md.


## How this is built

mwt is developed with Claude (Anthropic) as a coding assistant. I set the scope and the
architecture and review the changes; Claude writes most of the code and the tests. The
checks:

* `make check` runs the unit tests of the VT core, of the mwtrc and the colors, of what
  keys and the mouse send, of what a paste becomes, of the selection, of URLs, of the
  shell on its pty and of `-g`, and the kept fuzz findings, under ASan and UBSan, and
  checks mwt's command line. `make fuzz` searches for new findings with libFuzzer, and `make
  replay` plays about 290 recorded terminal files through the core.
* `make test`, `make keys`, `make bell`, `make links`, `make wayland`, `make pixel`,
  `make mouse` and `make keyboard` drive mwt under a headless sway with a simulated
  mouse and keyboard, and check what it sends to the program and what it draws.
* `make evil` runs all of it in a container, with sanitizers, leak checks and
  adversarial cases such as floods, stalled pipes and running out of memory.
* `make terminfo` checks that `mwt.info` promises only what mwt does.
* Every fix comes with a test that fails without it, checked by taking the fix out.
* What is left open is written down rather than hidden: see CAVEATS.md, TODO, and the
  reviews in `docs/history`.


## In mwc

Frames come through xdg-decoration. xdg-shell has no size step, so mwt snaps a floating
window to whole cells itself; maximized, fullscreen or tiled it takes the size it is
given and fills the rest with the background. mwc's resize box therefore shows pixels,
not columns and rows.
