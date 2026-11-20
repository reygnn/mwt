#!/usr/bin/env python3
"""Builds the font collection tests/font_ttc.c needs: two small monospace fonts in
one .ttc, "Mwt TTC A" at index 0 and "Mwt TTC B" at index 1. Each has ASCII and
a character of its own no other font has, U+10FFF0 in A and U+10FFF1 in B, so
one is drawn from the other face of the same file. Glyphs are boxes. Run by
tests/Containerfile.fonts: python3 mkttc.py OUT.ttc"""
import sys

from fontTools.fontBuilder import FontBuilder
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.ttLib import TTCollection

OWN = {"Mwt TTC A": 0x10FFF0, "Mwt TTC B": 0x10FFF1}


def box(empty=False):
    pen = TTGlyphPen(None)
    if not empty:
        pen.moveTo((100, 0))
        pen.lineTo((100, 700))
        pen.lineTo((500, 700))
        pen.lineTo((500, 0))
        pen.closePath()
    return pen.glyph()


def build(family, own):
    cmap = {c: "u%04X" % c for c in range(0x20, 0x7F)}
    cmap[own] = "own"
    names = [".notdef"] + sorted(set(cmap.values()))
    fb = FontBuilder(1000, isTTF=True)
    fb.setupGlyphOrder(names)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf({n: box(n == "u0020") for n in names})
    fb.setupHorizontalMetrics({n: (600, 100) for n in names})
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": family, "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, usWinAscent=800, usWinDescent=200)
    fb.setupPost(isFixedPitch=1)
    return fb.font


def main():
    ttc = TTCollection()
    ttc.fonts = [build(family, own) for family, own in OWN.items()]
    ttc.save(sys.argv[1])


if __name__ == "__main__":
    main()
