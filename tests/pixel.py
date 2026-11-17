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
             + b"\033[41mHH\033[0m "                # 30 a background below a glyph
             + b"\033[4;9;8mHH\033[0m ")            # 33 invisible hides the lines too
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
    check(not g.ink(33, 0), "invisible hides underline and strike")


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


def test_zoom_tiled():
    """Tiled, the window keeps its size when the font changes, and the grid moves
    within it. Nothing of the old grid may stay beside the new one: at each step
    the screen red, the font one size up or down, then everything cleared to the
    default background. Whether a step leaves a strip depends on how the cell
    sizes divide the window, so it takes several steps each way."""
    with Window(geometry="30x8") as win:
        swaymsg('[app_id="mwt"]', "floating", "disable")
        time.sleep(0.5)
        left = []
        for key in ["minus"] * 3 + ["0"] + ["plus"] * 5:
            win.feed(RESET + b"\033[41m\033[2J")
            settle()
            combo(key, "ctrl", "shift")
            settle()
            win.feed(b"\033[0m\033[2J")
            img = settle()
            red = sum(img.px(x, y) == COLORS16[1] for y in range(img.h) for x in range(img.w))
            if red:
                left.append(f"{key}: {red}")
        combo("0", "ctrl", "shift")
        check(not left, "tiled, a new font size leaves nothing of the old grid",
              "red pixels after " + ", ".join(left))


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


def xcursor(path, sizes):
    """An Xcursor file with one square image per size, hotspot just past a quarter,
    where rounding and cutting off differ once a viewport scales it."""
    import struct
    toc = 16 + 12 * len(sizes)
    head, body = struct.pack("<4sIII", b"Xcur", 16, 0x10000, len(sizes)), b""
    for n in sizes:
        head += struct.pack("<III", 0xfffd0002, n, toc + len(body))
        body += struct.pack("<9I", 36, 0xfffd0002, n, 1, n, n, n // 4 + 1, n // 4 + 1, 0)
        body += b"\xff\x00\x00\xff" * (n * n)
    with open(path, "wb") as f:
        f.write(head + body)


def cursor_env(theme, size="24"):
    return {"XCURSOR_PATH": os.path.join(mwtlib.DIR, "icons"), "XCURSOR_THEME": theme,
            "XCURSOR_SIZE": size, "WAYLAND_DEBUG": "client"}


def cursor_theme(name, cursors):
    d = os.path.join(mwtlib.DIR, "icons", name, "cursors")
    os.makedirs(d, exist_ok=True)
    for c in cursors:
        xcursor(os.path.join(d, c), [24, 48, 64, 96])
    return d


def cursor_state(log):
    """From WAYLAND_DEBUG, in order: preferred scales, pointer enter and leave,
    set_cursor with its hotspot, and each commit of the cursor surface (the last one
    given to set_cursor) with what it holds: buffer size, buffer scale, viewport
    destination. Also a buffer destroyed while the cursor surface holds it, and a
    NULL attached to it."""
    lines = open(log, errors="replace").read().splitlines()
    cursor = None
    for line in lines:
        if " -> " in line and (m := re.search(r"set_cursor\(\d+, wl_surface#(\d+),", line)):
            cursor = m[1]
    bufs, scale, view, surf_of_view = {}, 1, None, {}
    held, states = None, []
    for line in lines:
        if " -> " not in line:
            if "preferred_buffer_scale(" in line:
                states.append(("scale", int(re.search(r"\((\d+)\)", line)[1])))
            elif m := re.search(r"wl_pointer#\d+\.(enter|leave)\(", line):
                states.append((m[1],))
            continue
        if m := re.search(r"create_buffer\(new id wl_buffer#(\d+), -?\d+, (\d+), (\d+),", line):
            bufs[m[1]] = (int(m[2]), int(m[3]))
        elif m := re.search(r"wl_buffer#(\d+)\.destroy\(\)", line):
            if m[1] == held:
                states.append(("destroyed_held", bufs.get(m[1])))
        elif m := re.search(r"wl_surface#(\d+)\.set_buffer_scale\((\d+)\)", line):
            if m[1] == cursor:
                scale = int(m[2])
        elif m := re.search(r"get_viewport\(new id wp_viewport#(\d+), wl_surface#(\d+)\)", line):
            surf_of_view[m[1]] = m[2]
        elif m := re.search(r"wp_viewport#(\d+)\.set_destination\((-?\d+), (-?\d+)\)", line):
            if surf_of_view.get(m[1]) == cursor:
                view = (int(m[2]), int(m[3]))
        elif m := re.search(r"wp_viewport#(\d+)\.destroy\(\)", line):
            if surf_of_view.pop(m[1], None) == cursor:
                view = None
        elif m := re.search(r"wl_surface#(\d+)\.attach\((nil|wl_buffer#(\d+))", line):
            if m[1] == cursor:
                held = m[3]
                if m[2] == "nil":
                    states.append(("attach_nil",))
        elif m := re.search(r"set_cursor\(\d+, wl_surface#\d+, (-?\d+), (-?\d+)\)", line):
            states.append(("set_cursor", (int(m[1]), int(m[2]))))
        elif (m := re.search(r"wl_surface#(\d+)\.commit\(\)", line)) and m[1] == cursor:
            states.append(("commit", bufs.get(held), scale, view))
    return states


def test_cursor_scale():
    """The cursor at scale 3 from a theme that has 24, 48, 64 and 96 pixels, as Adwaita:
    a buffer scale must divide the buffer, or the surface is in error. It is set anew
    when the scale changes, not only on the next enter, and the cursor surface never
    holds a buffer of a theme already destroyed, with the pointer on the window or
    away from it."""
    cursor_theme("squares", ["text"])
    log = os.path.join(mwtlib.DIR, "wayland-debug")
    mouse = Mouse()
    with Window(geometry="30x8", env=cursor_env("squares"), stderr=log):
        x, y, w, h = rect()
        mouse.send("move", x + w // 2, y + h // 2)
        time.sleep(0.3)
        for s in (2, 3, 1):
            swaymsg("output", "HEADLESS-1", "scale", str(s))
            time.sleep(0.8)
        mouse.send("move", 999, 699)
        time.sleep(0.3)
        for s in (2, 1):
            swaymsg("output", "HEADLESS-1", "scale", str(s))
            time.sleep(0.8)
        settle()
    mouse.close()
    swaymsg("output", "HEADLESS-1", "scale", "1")
    states = cursor_state(log)
    commits = [st for st in states if st[0] == "commit"]
    bad = [st for st in commits if st[1] and st[3] is None and (st[1][0] % st[2] or st[1][1] % st[2])]
    check(commits and not bad, "the cursor's buffer scale divides its buffer", f"{bad or states}")
    gone = [st for st in states if st[0] == "destroyed_held"]
    check(not gone, "the cursor surface never holds a destroyed buffer", f"{gone} in {states}")
    first = next((i for i, st in enumerate(states) if st[0] == "scale" and st[1] != 1), len(states))
    nil = [st for st in states[first:] if st[0] == "attach_nil"]
    check(not nil, "the cursor surface is not emptied at a scale change", f"{states}")
    # After each preferred scale: a commit of the new image, and with the pointer
    # inside, a set_cursor for the new hotspot before any new enter.
    inside, after = False, []
    for st in states:
        if st[0] in ("enter", "leave"):
            inside = st[0] == "enter"
        if st[0] == "scale":
            after.append((st[1], []))
        elif after:
            after[-1][1].append((*st, inside) if st[0] == "commit" else st)
    want = [(2, (48, 48), 2, None, (7, 7)), (3, (64, 64), 1, (21, 21), (6, 6)),
            (1, (24, 24), 1, None, (7, 7)), (2, (48, 48), 2, None, None), (1, (24, 24), 1, None, None)]
    if not check(len(after) >= len(want) and [a[0] for a in after[-5:]] == [w[0] for w in want],
                 "the scale changes arrive", f"{[a[0] for a in after]}"):
        return
    for (s, got), (_, size, bscale, dest, hot) in zip(after[-5:], want):
        ci = [i for i, st in enumerate(got) if st[0] == "commit"]
        where = "on the window" if hot else "away"
        if not ci:
            fail(f"scale {s}, pointer {where}: the cursor is not loaded anew: {got}")
            continue
        commit, rest = got[ci[-1]], got[ci[-1] + 1:]
        if commit[4]:
            rest = rest[:next((i for i, st in enumerate(rest) if st[0] == "enter"), len(rest))]
        hs = [st[1] for st in rest if st[0] == "set_cursor"]
        check(commit[1:4] == (size, bscale, dest) and (hot is None or hs and hs[0] == hot),
              f"scale {s}, pointer {where}: the cursor is {size[0]} pixels at scale {bscale}"
              + (f", hotspot {hot}, at once" if hot else ""), f"{got}")


def test_cursor_size():
    """A huge XCURSOR_SIZE gives the largest cursor, also at scale 3, where size times
    scale would overflow an int."""
    cursor_theme("squares", ["text"])
    log = os.path.join(mwtlib.DIR, "wayland-debug-size")
    mouse = Mouse()
    with Window(geometry="30x8", env=cursor_env("squares", "1000000000"), stderr=log):
        x, y, w, h = rect()
        mouse.send("move", x + w // 2, y + h // 2)
        time.sleep(0.3)
        swaymsg("output", "HEADLESS-1", "scale", "3")
        time.sleep(0.8)
        settle(scale=3)
        mouse.send("move", 999, 699)
    mouse.close()
    swaymsg("output", "HEADLESS-1", "scale", "1")
    time.sleep(0.5)
    commits = [st for st in cursor_state(log) if st[0] == "commit"]
    check(commits and commits[-1][1] == (96, 96), "XCURSOR_SIZE=1000000000 at scale 3: the largest cursor",
          f"{commits}")


def test_cursor_retry():
    """A theme without a text cursor at the start; once it has one, the next enter
    loads it."""
    d = cursor_theme("partial", ["wait"])
    log = os.path.join(mwtlib.DIR, "wayland-debug-retry")
    mouse = Mouse()
    mouse.send("move", 999, 699)
    with Window(geometry="30x8", env=cursor_env("partial"), stderr=log):
        xcursor(os.path.join(d, "text"), [24, 48, 64, 96])
        x, y, w, h = rect()
        mouse.send("move", x + w // 2, y + h // 2)
        time.sleep(0.5)
        mouse.send("move", 999, 699)
        time.sleep(0.2)
    mouse.close()
    states = cursor_state(log)
    commits = [st for st in states if st[0] == "commit"]
    sets = [st for st in states if st[0] == "set_cursor"]
    check(commits and commits[-1][1] == (24, 24) and sets and sets[-1][1] == (7, 7),
          "a failed cursor load is tried again on the next enter", f"{states}")


def test_mwtrc():
    """Colors from $XDG_CONFIG_HOME/mwt/mwtrc over those of config.h, per app_id too."""
    fg, bg, other_bg, blue = 0xe5e5e5, 0x000040, 0x000060, 0x88bbe0
    d = os.path.join(os.environ["XDG_CONFIG_HOME"], "mwt")
    os.makedirs(d, exist_ok=True)
    rc = os.path.join(d, "mwtrc")
    with open(rc, "w") as f:
        f.write("! IRIS Indigo\n"
                f"Mwt*foreground: #{fg:06x}\n"
                f"Mwt*background: #{bg:06x}\n"
                "Mwt*bogus: #000000\n"
                f"Mwt*color4: #{blue:06x}\n"
                f"Mwt*other*background: rgb:{other_bg >> 16:02x}/{other_bg >> 8 & 255:02x}/{other_bg & 255:02x}\n")
    try:
        with Window() as win:
            win.feed(b"\033cHH\033[2;1H\033[34mH\033[m\033[3;11H")
            g = Grid(settle(), 80, 24)
            check(g.corner(0, 5) == bg, "mwtrc: the background", hex6(g.corner(0, 5)))
            check(fg in g.colors(0, 0), "mwtrc: the text color")
            check(g.colors(10, 2) == {fg}, "mwtrc: the cursor follows the text color")
            check(blue in g.colors(0, 1), "mwtrc: color4 for SGR 34")
            img = g.img
            check(img.px(0, 0) == bg, "mwtrc: the padding takes the background")
        with Window("sleep", "100", app_id="other", geometry="10x2"):
            g = Grid(settle("other"), 10, 2)
            check(g.corner(5, 1) == other_bg, "mwtrc: a line for the app_id wins",
                  hex6(g.corner(5, 1)))
        with open(mwtlib.LOG, errors="replace") as f:
            check("mwtrc:4: unknown resource bogus" in f.read(), "mwtrc: a bad line is named")
    finally:
        os.remove(rc)


def test_contrast():
    """minimumContrast: color0 on color2 of paper, as htop draws its header, comes
    out in the cream of the background; at 0 it stays as it was."""
    fg, bg, green = 0x38322b, 0xf6efe3, 0x2a5e1e
    d = os.path.join(os.environ["XDG_CONFIG_HOME"], "mwt")
    os.makedirs(d, exist_ok=True)
    rc = os.path.join(d, "mwtrc")
    with open(rc, "w") as f:
        f.write(f"Mwt*foreground: #{fg:06x}\n"
                f"Mwt*background: #{bg:06x}\n"
                f"Mwt*color0: #{fg:06x}\n"
                f"Mwt*color2: #{green:06x}\n"
                "Mwt*off*minimumContrast: 0\n")
    try:
        draw = b"\033c\033[?25l\033[30;42mHH\033[m"
        with Window() as win:
            win.feed(draw)
            g = Grid(settle(), 80, 24)
            c = g.colors(0, 0)
            check(g.corner(0, 0) == green and bg in c and fg not in c,
                  "minimumContrast: black on green drawn in cream", " ".join(map(hex6, sorted(c))))
        with Window(app_id="off") as win:
            win.feed(draw)
            g = Grid(settle("off"), 80, 24)
            c = g.colors(0, 0)
            check(fg in c, "minimumContrast: 0 leaves black on green",
                  " ".join(map(hex6, sorted(c))))
    finally:
        os.remove(rc)


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
    test_zoom_tiled()
    test_hidpi()
    test_cursor_scale()
    test_cursor_size()
    test_cursor_retry()
    test_mwtrc()
    test_contrast()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
