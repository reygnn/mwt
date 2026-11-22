# Caveats

Behaviour of mwt that is meant that way or that mwt cannot change.


## The shell

`mwt -e` always exits with 0, also when the command fails or does not start. The error
message goes to the window, which closes at once.

If `$SHELL` is not executable or not an absolute path (`SHELL=bash`), the window closes
at once. Only an empty or unset `$SHELL` falls back to the passwd entry.

The shell gets mwt's environment as it is, apart from `TERM`, `COLORTERM`, `LINES` and
`COLUMNS`. Started from within tmux or kitty, `TMUX`, `TERM_PROGRAM` and the like come
along and mislead programs into tmux passthrough or kitty's protocols; unset them where
mwt is started.

When the window closes, the shell gets SIGHUP. mwt waits two seconds for it and then
exits. A shell that ignores the signal (nohup) or takes longer to clean up runs on
without a terminal; mwt does not kill it.

Output that floods the window, as `yes` or `cat` of a large file, is read in pieces with
a pause of 50 µs between short reads. That halves the CPU mwt takes, and such a flood
ends 25 to 35 % later than without the pause. An echo of typed input waits once.


## Window size

`-g` and the window are limited to a grid of 4096x2048 and a buffer under 2 GiB. With a
9x18 pixel font that is about 1800x1800 cells; `-g` refuses more with exit code 2, and
the grid stays at 4096x2048 in the largest window. A zoom that would pass 2 GiB is
refused, with a message on stderr.

mwt draws no frame of its own and uses no libdecor: frames are the compositor's job,
asked for through xdg-decoration. Where that is missing, as in GNOME and Weston, or the
compositor answers that the client draws them, the window has no frame, no title bar and
no border.

HiDPI needs `wl_compositor` version 6, which tells a surface its scale. Where a
compositor offers only an older one, mwt draws at scale 1, blurry on a HiDPI output.

There is no reflow: shrinking the window cuts off what no longer fits. A resize also
sets the tab stops back to every eighth column.


## Selection and paste

The selected text reaches only as far as the scrollback, 5000 lines (`config.h`).

A paste over 64 MiB is dropped whole, with a message on stderr. It counts as UTF-8,
which Latin-1 doubles, and together with what still waits for the program: a program
that does not read takes 64 MiB of pastes at most. Of the controls only tab, CR and LF
are pasted, LF and CR LF as CR; C1 controls are dropped as well, also as UTF-8.

With Ctrl+Shift+letter on non-Latin layouts without a Latin second layout, the key's
QWERTY position counts.


## Replies and the line discipline

Programs can ask the terminal for replies (DA, DSR). If the program exits first, the
replies end up on the shell's command line.

If nobody reads the replies, as with `cat` of a recorded file, the kernel's line
discipline echoes them back into the running output. That shows as `^[[>1;10;0c` on
the screen. If a piece of output ends in ESC and the echo after it starts with `c`, the
result is ESC c: a reset, which also leaves the alternate screen, so what follows lands
on the normal screen and in the history. mwt only sees the mixed stream and cannot tell
echo from output; neither can xterm. The remedy:

    stty -echo; cat file; stty echo

A program that stops output with `tcflow(TCOOFF)` is the only one that can start it
again. The stop is in the kernel; no key helps.

An unterminated DCS or OSC (ESC P or ESC ] without its end) swallows output up to the
next ESC, CAN or SUB, an OSC also up to BEL. bash usually ends it with the sequence of
its next prompt.


## Keypad mode

The application keypad mode (ESC =, ESC >, CSI ? 66 h and l) is kept and reported by
DECRQM but does not change the keys. With NumLock the digits send digits and Enter sends
`\r` (CR LF in newline mode); without it xkb gives KP_Home, KP_Up and so on, which send
the normal cursor and tilde sequences. This is xterm's behaviour with its default mode
1035. If the mode took effect, the keypad would send ESC O q instead of 1 as soon as a
curses program starts (`smkx` in `mwt.info` turns it on), and without `ka1`, `kent` and
the like in the terminfo no program would understand it. `kb2` in `mwt.info` is the
keypad's 5 without NumLock, ESC O E under `smkx` as cursor keys go; xterm's entries have
the keypad mode's ESC O u. With NumLock a keypad key is a cursor key only with Shift, so
`kUP5` and the like come from it only without NumLock; Ctrl+8 there sends DEL, Alt+8
ESC 8.


## Character widths

mwt counts as glibc and bash do, code point by code point with `wcwidth()`. Characters
without width (ZWJ, VS16, combining accents) are dropped, also when copying. tmux 3.6
counts ❤️ (with VS16) and ZWJ sequences such as 👨‍👩‍👧 differently, vim 9.1 flags and
Hangul jamo; there the cursor drifts. `make width` shows where. To a query for mode 2027
(CSI ? 2027 $ p) mwt answers "unknown"; see TODO.


## Sequences left out

DECRQM reports the modes mwt knows of but does not have as permanently reset (4).
Mode 1048 (save and restore the cursor) works, but DECRQM answers "unknown" for it:
xterm reports whether a cursor is saved, which mwt does not keep track of.

The cursor never blinks: the blinking styles of DECSCUSR (1, 3, 5) are drawn steady,
and DECRQM reports mode 12 as permanently reset.

Of SGR, blinking (5, 6) is kept but not drawn, double underline (21) is drawn single,
and the underline color (58) is read and ignored.

The wheel in the alternate screen sends arrow keys when no program has the mouse, as
xterm's alternate scroll mode (1007). In mwt it is always on: `CSI ? 1007 l` is ignored
and DECRQM answers "unknown".

Shift+PgUp and Shift+PgDn reach the program only in the alternate screen, Shift+Home
and Shift+End also when it has the mouse; otherwise they scroll the history. `kPRV`,
`kNXT`, `kHOM` and `kEND` in `mwt.info` hold only then. There is no `kIC`: Shift+Insert
always pastes. The keys of the keypad with NumLock and Shift always reach the program,
as in xterm.

Of the VT420 and xterm features esctest checks, these are left out, so most of what
`make esctest` reports fails by design: left and right margins (DECLRMM, DECSLRM),
rectangle operations (DECCRA, DECFRA, DECERA), inserting and deleting columns (DECIC,
DECDC, DECBI, DECFI), protected areas and selective erase (DECSCA, SPA, EPA, DECSED,
DECSEL), window operations beyond CSI 18 t, setting colors (OSC 4, 10 to 19, 104),
reverse wraparound (`?45`), 132 columns (DECCOLM), reverse screen (DECSCNM), double size
lines (`ESC # 3` to `6`), national character sets and G2 and G3, XTSAVE and XTRESTORE,
DECSCL, and the DECDSR reports for printer, keyboard and locator.

DA2 answers `CSI > 1 ; 10 ; 0 c`, a VT220 as DA does, with a version below xterm's:
esctest wants xterm's 41 and 314 or more, and programs like vim would take mwt for xterm.

Combining characters are discarded, see Character widths. Setting colors through OSC is
ignored; queries are answered.

REP past a screenful and a row leaves the screen as all the copies would, but puts fewer
lines in the history. DECSTR resets all of SGR, as DEC has it; xterm keeps faint, crossed
out and double underline.

A character that is not ASCII ends a sequence. xterm puts a combining mark on the
character before, ignores a C1, and reads U+00A0 to U+00FF as their ASCII counterparts.
SUB is dropped where xterm prints ␦.


## Input

mwt binds the first seat the compositor announces. Should that seat go away, another one
it announced already is not taken up; only a seat announced later is. Other globals
that go away (clipboard, primary selection, decorations, activation, the viewporter
for the mouse pointer) are kept; what mwt
asks of them then does nothing.

A mouse that goes off the seat with its button down on an item of the context menu runs
the item. sway sends the release itself, which mwt cannot tell from one let go; where a
compositor does not, mwt makes one up for each button held, and it runs the item too.

With Ctrl, a letter of a layout that is not Latin is taken as the Latin letter in its
place, so Ctrl+C interrupts on a Russian layout. Keys outside the three rows of letters
are not: with only such a layout in the keymap, Ctrl on them sends what the layout has
there: on a Ukrainian layout Ctrl+\ gives ґ rather than raising SIGQUIT. With a Latin
layout beside it, xkbcommon gives for Ctrl what that layout has on the key: ESC and GS
for the keys of [ and ] beside `us`, the letter and `+` beside `de`. Beside `us` that
holds for Latin layouts too: Ctrl+Shift on the key of § on `fr,us` is Ctrl+? there.

Ctrl+? sends DEL, unless Ctrl+Shift on the key of `?` is a shortcut of mwt: on the key of
`+`, `=`, `-` or `0` it zooms, in on the Danish, Finnish, Norwegian, Swedish, Estonian
and Faroese layouts, `pl(qwertz)` and `ro(winkeys)`, out on `fr(ergol)`, back on
Mongolian; on the key of A, C or V, where a layout that is not Latin may have `?`
(`ca(ike)`), it selects, copies or pastes.


## Bell

BEL makes no sound and does not flash. Without the focus the window asks for attention
through xdg-activation, once until it gets the focus back: sway marks it urgent, GNOME
shows it is ready. KWin gives it the focus instead when no window is active, or when its
focus stealing prevention is off. With the focus nothing happens, nor before the window
first shows or while mwt's menu is open. mwc marks the desktop of a window on another
desktop urgent (ext-workspace-v1) and shows nothing on the current one. Compositors
without xdg-activation show nothing.


## Fonts

A character the font has only in a bold or italic face, as U+27BF only in DejaVu Sans
Mono Bold, is drawn from another font in the other styles, and from that face only when
no font on the system has it.

A face that maps a character to an empty glyph counts as having it. DejaVu Sans Mono
Oblique does so for U+1D3D, which is blank in italic and bold italic.

A glyph of the font may reach a cell into its neighbours, as an italic does. Accents
above the font's ascent, as on Ǖ, are cut at the row.

A glyph from another font is drawn smaller where it does not fit its one or two cells,
and not at all where it would be less than a pixel high then: U+FDFD from Noto Sans
Arabic, about 12 cells wide, at 6 pt. Characters a monospaced font has come from it
rather than from a proportional one, but only as fontconfig tells them apart: Noto Sans
Mono sets no spacing and counts as proportional.

Box drawing and block elements, U+2500 to U+259F, and the DEC scan lines U+23BA to
U+23BD come from mwt and not from the font: lines in the middle of the cell, as thick as
the font's underline and at least a pixel a scale, heavy lines and light ones in bold
twice that, double lines two light strokes also in bold. A font's own design of them is
not used, nor its italic. Arcs and diagonals are thinner to the eye than straight lines,
being smoothed. A cell narrower than two pixels a dash runs some of the dashes of ┄ and ┈
together.

Out of memory a glyph may be blank. Left so by its bitmap or by the list of the system's
fonts, tried again at most once a second, it shows once its row is drawn again; where
loading the fallback face failed, only once the glyph cache empties or the font is
zoomed.


## Installation

`make install` puts the terminfo entry in `/usr/share/terminfo` whatever `PREFIX` is,
since ncurses does not look under `/usr/local`.

Without the entry mwt falls back to `TERM=xterm-256color`, which promises more than mwt
does: changing colors (`ccc`, `initc`, `oc`, `Cs`, `Cr`), a visual bell (`flash`,
reverse screen), meta mode (`km`, `smm`, `rmm`), blinking text (`blink`), left and right
margins (`smglr`, `smglp`, `smgrp`, `mgc`), the printer (`mc0`, `mc4`, `mc5`, `mc5i`),
memory lock (`meml`, `memu`), setting the selection (`Ms`), `kIC`, which pastes (see
Sequences left out), and the keypad keys `ka1` to `kc3`, which mwt sends as the normal
cursor keys (see Keypad mode). Install the entry.
