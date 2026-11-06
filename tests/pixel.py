#!/usr/bin/env python3
"""What mwt really draws: screenshots from grim, compared by the pixel. The other
tests read the screen through the selection, which sees text and no colors, and
none of what partial redraws leave behind. Colors come from config.h, so changing
them does not break this. Run with `make pixel`, through tests/headless.sh."""
import os
import random
import re
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, combo, finish, rect, settle, swaymsg

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESET = b"\033c\033[?25l"


def config():
    with open(os.path.join(ROOT, "config.h")) as f:
        s = f.read()
    named = {m[0]: int(m[1], 16) for m in re.findall(r"color_(\w+) = 0x([0-9a-f]+)", s)}
    table = re.search(r"colors16\[16\] = \{(.*?)\}", s, re.S).group(1)
    return named, [int(c, 16) for c in re.findall(r"0x([0-9a-f]+)", table)]


NAMED, COLORS16 = config()
FG, BG, CURSOR, SEL_BG = NAMED["fg"], NAMED["bg"], NAMED["cursor"], NAMED["sel_bg"]


def cube(i):
    """xterm's palette above the first 16."""
    if i >= 232:
        v = 8 + (i - 232) * 10
        return v << 16 | v << 8 | v
    i -= 16
    levels = [0, 95, 135, 175, 215, 255]
    return levels[i // 36] << 16 | levels[i // 6 % 6] << 8 | levels[i % 6]


def blend(bg, fg, a):
    out = 0
    for s in (16, 8, 0):
        out |= (((fg >> s & 255) * a + (bg >> s & 255) * (255 - a)) // 255) << s
    return out


class Grid:
    """Pixels of a cell, for a window of cols x rows and a screenshot."""

    def __init__(self, img, cols, rows):
        s = img.scale
        self.img = img
        self.cw = (img.w - 2 * mwtlib.PAD * s) // cols
        self.ch = (img.h - 2 * mwtlib.PAD * s) // rows
        self.pad = mwtlib.PAD * s

    def box(self, col, row):
        x0, y0 = self.pad + col * self.cw, self.pad + row * self.ch
        return x0, y0, x0 + self.cw, y0 + self.ch

    def corner(self, col, row):
        x0, y0, _, _ = self.box(col, row)
        return self.img.px(x0, y0)

    def colors(self, col, row):
        x0, y0, x1, y1 = self.box(col, row)
        return {self.img.px(x, y) for y in range(y0, y1) for x in range(x0, x1)}

    def full_rows(self, col, row, color):
        x0, y0, x1, y1 = self.box(col, row)
        return [y - y0 for y in range(y0, y1) if all(self.img.px(x, y) == color for x in range(x0, x1))]

    def full_cols(self, col, row, color):
        x0, y0, x1, y1 = self.box(col, row)
        return [x - x0 for x in range(x0, x1) if all(self.img.px(x, y) == color for y in range(y0, y1))]

    def ink(self, col, row):
        return self.colors(col, row) != {self.corner(col, row)}


def hex6(c):
    return f"{c:06x}"


def test_colors(win):
    seq = RESET
    want = []
    for i in range(16):
        seq += b"\033[48;5;%dm " % i
        want.append((f"palette {i}", COLORS16[i]))
    for i in (16, 21, 46, 196, 231, 232, 244, 255):
        seq += b"\033[48;5;%dm " % i
        want.append((f"palette {i}", cube(i)))
    for i in range(8):
        seq += b"\033[%dm " % (40 + i)
        want.append((f"SGR {40 + i}", COLORS16[i]))
        seq += b"\033[%dm " % (100 + i)
        want.append((f"SGR {100 + i}", COLORS16[8 + i]))
    seq += b"\033[48;2;1;2;3m "
    want.append(("truecolor with ;", 0x010203))
    seq += b"\033[48:2::250:128:7m "
    want.append(("truecolor with :", 0xfa8007))
    seq += b"\033[49m "
    want.append(("SGR 49", BG))
    win.feed(seq)
    g = Grid(settle(), 80, 24)
    bad = [f"{name}: {hex6(g.corner(x, 0))}, expected {hex6(c)}"
           for x, (name, c) in enumerate(want) if g.corner(x, 0) != c]
    check(not bad, f"{len(want)} background colors", "; ".join(bad))
    check(g.corner(0, 5) == BG, "the default background")


def test_attributes(win):
    win.feed(RESET
             + b"\033[4m  \033[0m "                 # 0 underline
             + b"\033[9m  \033[0m "                 # 3 strike
             + b"\033[4;31m  \033[0m "              # 6 underline takes the color
             + b"\033[2;4m  \033[0m "               # 9 dim
             + b"\033[7m  \033[0m "                 # 12 reverse
             + b"\033[7;31m  \033[0m "              # 15 reverse with a color
             + b"\033[41;7;32m  \033[0m "           # 18 reverse swaps both
             + b"HH "                               # 21 plain
             + b"\033[8mHH\033[0m "                 # 24 invisible
             + b"\033[1mHH\033[0m "                 # 27 bold
             + b"\033[41mHH\033[0m ")               # 30 a background below a glyph
    img = settle()
    g = Grid(img, 80, 24)
    under = g.full_rows(0, 0, FG)
    check(len(under) == 1, "underline is one pixel row", str(under))
    strike = g.full_rows(3, 0, FG)
    check(len(strike) == 1, "strike is one pixel row", str(strike))
    check(strike and under and strike[0] < under[0], "strike is above the underline", f"{strike} {under}")
    check(len(g.full_rows(6, 0, COLORS16[1])) == 1, "underline takes the color")
    dim = blend(BG, FG, 140)
    check(len(g.full_rows(9, 0, dim)) == 1, "dim blends into the background", f"no row of {hex6(dim)}")
    check(g.colors(12, 0) == {FG}, "reverse: the foreground fills the cell")
    check(g.colors(15, 0) == {COLORS16[1]}, "reverse with a color")
    check(g.colors(18, 0) == {COLORS16[2]}, "reverse swaps a set background too")
    check(g.ink(21, 0), "a plain H shows")
    check(not g.ink(24, 0), "invisible shows nothing")
    check(g.colors(27, 0) != g.colors(21, 0), "bold looks different")
    check(g.corner(30, 0) == COLORS16[1] and g.ink(30, 0), "a glyph over a background")


def test_cursor(win):
    win.feed(b"\033c\033[3;11H")
    g = Grid(settle(), 80, 24)
    check(g.colors(10, 2) == {CURSOR}, "block cursor fills the cell")
    win.feed(b"\033[4;21H")
    g = Grid(settle(), 80, 24)
    check(g.colors(20, 3) == {CURSOR}, "the cursor moves")
    check(g.colors(10, 2) == {BG}, "and leaves nothing behind")
    win.feed(b"\033[4 q")
    g = Grid(settle(), 80, 24)
    rows = g.full_rows(20, 3, CURSOR)
    check(rows == [g.ch - 2, g.ch - 1], "underline cursor: the last two pixel rows", str(rows))
    win.feed(b"\033[6 q")
    g = Grid(settle(), 80, 24)
    cols = g.full_cols(20, 3, CURSOR)
    check(cols == [0, 1], "bar cursor: the first two pixel columns", str(cols))
    win.feed(b"\033[2 q\033[?25l")
    g = Grid(settle(), 80, 24)
    check(g.colors(20, 3) == {BG}, "a hidden cursor does not show")
    win.feed(b"\033[?25h\033[1;1H\xe6\xbc\xa2\033[1;1H")
    g = Grid(settle(), 80, 24)
    check(g.corner(0, 0) == CURSOR and g.corner(1, 0) == CURSOR, "on a wide character it is two cells wide")
    win.feed(b"\033c\033[3;11H")
    settle()
    with Window("sleep", "100", app_id="other", geometry="10x2"):
        g = Grid(settle(), 80, 24)
        x0, y0, x1, y1 = g.box(10, 2)
        px = g.img.px
        outline = (px(x0, y0) == CURSOR and px(x1 - 1, y1 - 1) == CURSOR and
                   px(x0, y1 - 1) == CURSOR and px(x1 - 1, y0) == CURSOR)
        check(outline and px((x0 + x1) // 2, (y0 + y1) // 2) == BG, "without focus an outline")
    g = Grid(settle(), 80, 24)
    check(g.colors(10, 2) == {CURSOR}, "with focus back a block again")


def test_selection(win, mouse):
    win.feed(RESET + b"zeile eins\r\nzeile zwei\r\n")
    before = settle()
    g = Grid(before, 80, 24)
    x, y, _, _ = rect()

    def at(col, row):
        x0, y0, x1, y1 = g.box(col, row)
        mouse.send("move", x + (x0 + x1) // 2, y + (y0 + y1) // 2)

    time.sleep(0.5)
    at(2, 0)
    mouse.send("down", "left")
    at(7, 1)
    mouse.send("up", "left")
    sel = Grid(settle(), 80, 24)
    check(sel.corner(2, 0) == SEL_BG and sel.corner(40, 0) == SEL_BG, "selection: the first row to its end")
    check(sel.corner(7, 1) == SEL_BG and sel.corner(0, 1) == SEL_BG, "selection: the last row from its start")
    check(sel.corner(1, 0) == BG and sel.corner(8, 1) == BG, "selection: nothing outside")
    check(sel.colors(2, 0) != {SEL_BG}, "selection: the text stays readable")
    time.sleep(0.5)
    at(40, 10)
    mouse.send("click", "left")
    check(settle() == before, "selection: a click takes it back, pixel for pixel")


def test_scrollback(win):
    lines = b"".join(b"\033[3%dmzeile %d\033[0m\r\n" % (i % 8, i) for i in range(300))
    win.feed(RESET + lines)
    before = settle()
    combo("Prior", "shift")
    up = settle()
    check(up != before, "Shift+PgUp changes the picture")
    combo("Prior", "shift")
    combo("Next", "shift")
    check(settle() == up, "up and down again gives the same picture")
    combo("End", "shift")
    check(settle() == before, "Shift+End gives back the live screen, pixel for pixel")


def noise(seed):
    """Output that moves things about, in pieces, without leaving a sequence open."""
    r = random.Random(seed)
    text = ["abc", "Hallo Welt", "\xe6\xbc\xa2\xe5\xad\x97".encode("latin1").decode(), "x" * 90, "äöü€"]

    def piece():
        k = r.randrange(26)
        y, x = r.randrange(1, 25), r.randrange(1, 81)
        n = r.randrange(1, 6)
        if k < 6:
            return r.choice(text).encode()
        return {
            6: b"\033[%d;%dH" % (y, x), 7: b"\033[%d;%dH" % (y, x), 8: b"\r\n", 9: b"\r\n",
            10: b"\033[%dm" % r.choice([0, 1, 2, 4, 7, 9, 31, 42, 95, 106]),
            11: b"\033[38;5;%dm" % r.randrange(256), 12: b"\033[48;2;%d;%d;%dm" % (r.randrange(256), 40, 90),
            13: b"\033[K", 14: b"\033[1K", 15: b"\033[%d@" % n, 16: b"\033[%dP" % n, 17: b"\033[%dX" % n,
            18: b"\033[%dL" % n, 19: b"\033[%dM" % n, 20: b"\033[%dS" % n, 21: b"\033[%dT" % n,
            22: b"\033[%d;%dr" % (min(y, 12), max(y, 13)), 23: b"\033[r",
            24: r.choice([b"\033[?1049h", b"\033[?1049l"]), 25: r.choice([b"\033[?25l", b"\033[?25h"]),
        }[k]

    return [piece() for _ in range(400)]


def diff_cells(a, b):
    g = Grid(a, 80, 24)
    cells = set()
    for y in range(a.h):
        ra, rb = a.data[y * a.w * 3:(y + 1) * a.w * 3], b.data[y * a.w * 3:(y + 1) * a.w * 3]
        if ra != rb:
            for x in range(a.w):
                if ra[x * 3:x * 3 + 3] != rb[x * 3:x * 3 + 3]:
                    cells.add(((y - g.pad) // g.ch, (x - g.pad) // g.cw))
    return sorted(cells)


def test_partial_redraw():
    """Piece by piece, so that each frame draws only some rows on top of the last,
    against all at once in a new window. They must not differ by a pixel."""
    for seed in (1, 2, 3, 4):
        pieces = noise(seed)
        with Window() as win:
            win.feed(b"\033c")
            for i, p in enumerate(pieces):
                win.feed(p)
                if i % 3 == 0:
                    time.sleep(0.02)
            slow = settle()
        with Window() as win:
            win.feed(b"\033c" + b"".join(pieces))
            fast = settle()
        same = slow == fast
        detail = "" if same else f"cells (row, column) {diff_cells(slow, fast)[:12]}"
        check(same, f"partial redraws, seed {seed}: the same as one full draw", detail)


def red_rows(img):
    """How many pixel rows the red first row of the test image has."""
    x = img.w // 2
    return sum(img.px(x, y) == COLORS16[1] for y in range(img.h))


def test_zoom():
    with Window(geometry="30x8") as win:
        win.feed(RESET + b"\033[41m\033[Ktext\033[0m\r\nzweite Zeile")
        before = settle()
        size = rect()[2:]
        rows0 = red_rows(before)
        combo("plus", "ctrl", "shift")
        big = settle()
        check(rect()[2] > size[0] and rect()[3] > size[1], "Ctrl+Shift+Plus makes the window larger", f"{size} to {rect()[2:]}")
        check(red_rows(big) > rows0, "and the rows higher", f"{rows0} to {red_rows(big)}")
        combo("0", "ctrl", "shift")
        check(settle() == before, "Ctrl+Shift+0 gives back the same picture")
        combo("minus", "ctrl", "shift")
        settle()
        check(rect()[2] < size[0] and rect()[3] < size[1], "Ctrl+Shift+Minus makes the window smaller", f"{size} to {rect()[2:]}")
        combo("0", "ctrl", "shift")
        check(settle() == before, "and Ctrl+Shift+0 again")


def test_hidpi():
    with Window(geometry="30x8") as win:
        win.feed(RESET + b"\033[41m\033[Ktext\033[0m\r\n\033[4m    \033[0m plain")
        one = settle()
        rows1 = red_rows(one)
        under1 = Grid(one, 30, 8).full_rows(0, 1, FG)
        swaymsg("output", "HEADLESS-1", "scale", "2")
        time.sleep(0.5)
        two = settle(scale=2)
        check((two.w, two.h) == (2 * rect()[2], 2 * rect()[3]), "scale 2: the picture has twice the pixels",
              f"{two.w}x{two.h} for {rect()[2:]}")
        g = Grid(two, 30, 8)
        under2 = g.full_rows(0, 1, FG)
        check(len(under1) == 1 and len(under2) == 2, "scale 2: a line is two pixels", f"{under1} {under2}")
        rows2 = red_rows(two)
        check(abs(rows2 - 2 * rows1) <= 2, "scale 2: a row has about twice the pixels", f"{rows1} to {rows2}")
        check(rows2 % 2 == 0 or g.corner(0, 0) == COLORS16[1], "scale 2: the color holds", hex6(g.corner(0, 0)))
        check(g.colors(5, 1) != {g.corner(5, 1)} or g.corner(5, 1) == BG, "scale 2: the text draws")
        # Glyphs rendered for the scale, not a stretched picture: stretched, every
        # pixel would equal its neighbor in pairs.
        x0, y0, x1, y1 = g.box(0, 0)
        odd = sum(two.px(x, y) != two.px(x + 1 - (x - x0) % 2 * 2, y) for y in range(y0, y1)
                  for x in range(x0, x1 - 1))
        check(odd > 0, "scale 2: the glyphs are not stretched")
        swaymsg("output", "HEADLESS-1", "scale", "1")
        time.sleep(0.5)
        check(settle() == one, "back at scale 1 the same picture")


def main():
    # Its virtual keyboard keeps one on the seat. Without one no window has focus,
    # and the cursor is only ever an outline.
    mouse = Mouse()
    with Window() as win:
        test_colors(win)
        test_attributes(win)
        test_cursor(win)
        test_selection(win, mouse)
        test_scrollback(win)
    test_partial_redraw()
    test_zoom()
    test_hidpi()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
