#!/usr/bin/env python3
"""Glyphs from fallback faces, which only the image of `make fonts` has: the image
of `make evil` has DejaVu alone, and there find_fallback in font.c never finds a
face. Screenshots as in tests/pixel.py. The timings describe rather than judge and
print info: what a glyph no face has costs, the data for deciding on a negative
cache (see docs/history/Review-2026-11-08.md). FALLBACK_NO_TIMING=1 leaves
them out, for a sanitizer build. Run with `make fonts`, through tests/headless.sh."""
import os
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, combo, finish, settle
from pixel import BG, RESET, Grid

CJK = "漢"      # 漢, in Droid Sans Fallback
HANGUL = "한"   # 한, in Nanum: Droid Sans Fallback has no Hangul
EMOJI = "\U0001f642"  # 🙂, a color bitmap in Noto Color Emoji, an outline in Symbola
# A letter from each of many scripts, each of which fonts-noto-core has in a file of
# its own, more of them than the 16 fallback faces mwt keeps open.
SCRIPTS = [
    ("Ethiopic", "አ"), ("Cherokee", "Ꭰ"), ("Thaana", "ހ"),
    ("Sinhala", "අ"), ("Khmer", "ក"), ("Lao", "ກ"),
    ("Myanmar", "က"), ("Tibetan", "ཀ"), ("Mongolian", "ᠠ"),
    ("Tifinagh", "ⴰ"), ("Gujarati", "અ"), ("Gurmukhi", "ਅ"),
    ("Kannada", "ಅ"), ("Telugu", "అ"), ("Malayalam", "അ"),
    ("Oriya", "ଅ"), ("Bengali", "অ"), ("Tamil", "அ"),
    ("Devanagari", "अ"), ("Thai", "ก"), ("Syriac", "ܐ"),
    ("NKo", "ߊ"), ("Javanese", "ꦄ"), ("Balinese", "ᬅ"),
    ("Vai", "ꔀ"), ("Canadian syllabics", "ᐁ"),
]


def ink_pixels(g, col, row):
    x0, y0, x1, y1 = g.box(col, row)
    return sum(g.img.px(x, y) != BG for y in range(y0, y1) for x in range(x0, x1))


def test_wide(win):
    """A wide glyph from a fallback face, in both its cells, and bold; Hangul from a
    second fallback face, as the first has none."""
    win.feed(RESET + f"{CJK} {HANGUL}\r\n\033[1m{CJK}\033[0m".encode())
    g = Grid(settle(), 80, 24)
    check(g.ink(0, 0) and g.ink(1, 0), "CJK from a fallback face fills both cells")
    check(g.ink(3, 0) or g.ink(4, 0), "Hangul from a fallback face")
    regular = ink_pixels(g, 0, 0) + ink_pixels(g, 1, 0)
    bold = ink_pixels(g, 0, 1) + ink_pixels(g, 1, 1)
    check(bold > regular, "bold CJK from a fallback face is emboldened", f"{regular} to {bold} pixels")


def test_emoji(win):
    """The README promises emoji as outlines if a fallback face has them. Here
    fontconfig may rank the bitmap face first, which load_face refuses, and
    find_fallback takes the best match only."""
    win.feed(RESET + EMOJI.encode())
    g = Grid(settle(), 80, 24)
    check(g.ink(0, 0) or g.ink(1, 0), "an emoji draws as an outline though a bitmap face has it too")


def test_zoom(win):
    """Zooming closes the font and its fallback faces; the glyph must come back."""
    win.feed(RESET + CJK.encode())
    settle()
    combo("plus", "ctrl", "shift")
    g = Grid(settle(), 80, 24)
    check(g.ink(0, 0) and g.ink(1, 0), "CJK after a zoom, from a fallback face loaded anew")
    combo("0", "ctrl", "shift")
    settle()


def test_many_scripts():
    """More scripts than the 16 fallback faces mwt keeps open: each must draw, as
    the face used longest ago makes room. Then the first ones in bold, a style not
    drawn yet, which needs their faces again after they made room."""
    with Window() as win:
        win.feed(RESET + " ".join(c for _, c in SCRIPTS).encode() + b"\r\nX\r\n\033[1m"
                 + " ".join(c for _, c in SCRIPTS[:8]).encode() + b"\033[0m")
        g = Grid(settle(), 80, 24)
        drawn = [name for i, (name, _) in enumerate(SCRIPTS) if g.ink(2 * i, 0)]
        missing = [name for name, _ in SCRIPTS if name not in drawn]
        check(g.ink(0, 1), "the text after them still draws")
        check(not missing, f"all {len(SCRIPTS)} scripts from fallback faces, more than 16",
              f"none for {', '.join(missing)}")
        bold = [name for i, (name, _) in enumerate(SCRIPTS[:8]) if not g.ink(2 * i, 2)]
        check(not bold, "the first 8 again in bold, from faces loaded anew",
              f"none for {', '.join(bold)}")


def until_marker(win, text, limit=60.0):
    """Seconds from writing a screenful that ends in an X in the last cell until the
    X shows, or None. The frame with the X draws all the rest as well."""
    win.feed(RESET)
    settle()
    data = b"\033[H" + text.encode() + b"X"
    t0 = time.time()
    win.feed(data)
    while time.time() - t0 < limit:
        if Grid(mwtlib.shot(), 80, 24).ink(79, 23):
            return time.time() - t0
        time.sleep(0.01)
    return None


def test_missing_cost():
    """A glyph no face has costs a FcFontMatch the first time, in each style. A
    window of its own, with no fallback face open yet. Code points of a private
    use plane, which no font here has."""
    n = 80 * 24 - 1
    with Window() as win:
        ascii_s = until_marker(win, "a" * n)
        first = until_marker(win, "".join(chr(0xF0000 + i) for i in range(n)))
        again = until_marker(win, "".join(chr(0xF0000 + i) for i in range(n)))
        other = until_marker(win, "".join(chr(0xF0000 + n + i) for i in range(n)))
    ok = None not in (ascii_s, first, again, other)
    check(ok, "a screenful of glyphs no face has is drawn within 60 s")
    if ok:
        print(f"info: a screenful: ASCII {ascii_s:.2f} s, {n} code points no face has "
              f"{first:.2f} s, the same again {again:.2f} s, {n} others {other:.2f} s")


def main():
    # As in tests/pixel.py: vinput's keyboard keeps one on the seat, or no window
    # has focus for the zoom keys.
    mouse = Mouse()
    with Window() as win:
        test_wide(win)
        test_emoji(win)
        test_zoom(win)
    test_many_scripts()
    if not os.environ.get("FALLBACK_NO_TIMING"):
        test_missing_cost()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
