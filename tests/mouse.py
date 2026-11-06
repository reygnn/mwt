#!/usr/bin/env python3
"""What mwt reports for the mouse in each mode, byte for byte: buttons, motion,
the wheel and modifiers, in SGR and in the old X10 form. A program in raw mode
logs what it gets, as in tests/keys.py. Run with `make mouse`, which goes through
tests/headless.sh. Selection and the menu under Shift are in tests/run.sh."""
import sys
import time

import mwtlib
from mwtlib import Mouse, check, finish, rect
from keys import Logger

mouse = None


def at(col, row, cols=80, rows=24):
    x, y, w, h = rect()
    cw, ch = (w - 2 * mwtlib.PAD) // cols, (h - 2 * mwtlib.PAD) // rows
    mouse.send("move", x + mwtlib.PAD + col * cw + cw // 2, y + mwtlib.PAD + row * ch + ch // 2)
    time.sleep(0.05)


def sgr(code, col, row, press=True):
    return b"\033[<%d;%d;%d%c" % (code, col + 1, row + 1, b"M"[0] if press else b"m"[0])


def x10(code, col, row):
    return b"\033[M" + bytes((32 + code, 33 + col, 33 + row))


def expect(log, what, want):
    """Exactly these bytes, and nothing more behind them."""
    got = log.take(want)
    time.sleep(0.15)
    got += log.take(b"")
    check(got == want, what, f"got {got!r}, expected {want!r}")


def session(init, **kw):
    log = Logger(init, **kw)
    log.take()
    return log


def test_buttons_sgr():
    log = session(b"\033[?1000;1006h")
    at(7, 3)
    for name, code in (("left", 0), ("middle", 1), ("right", 2)):
        mouse.send("down", name)
        expect(log, f"1000 SGR: {name} press", sgr(code, 7, 3))
        mouse.send("up", name)
        expect(log, f"1000 SGR: {name} release", sgr(code, 7, 3, False))
    at(20, 10)
    expect(log, "1000: moving reports nothing", b"")
    mouse.send("down", "left")
    log.take(sgr(0, 20, 10))
    at(25, 12)
    expect(log, "1000: dragging reports nothing", b"")
    mouse.send("up", "left")
    expect(log, "1000: the release is where the pointer is", sgr(0, 25, 12, False))
    log.close()


def test_edges():
    log = session(b"\033[?1000;1006h")
    x, y, w, h = rect()
    mouse.send("move", x, y)
    time.sleep(0.05)
    mouse.send("click", "left")
    expect(log, "the padding at the top left counts as the first cell", sgr(0, 0, 0) + sgr(0, 0, 0, False))
    mouse.send("move", x + w - 1, y + h - 1)
    time.sleep(0.05)
    mouse.send("click", "left")
    expect(log, "the padding at the bottom right counts as the last cell", sgr(0, 79, 23) + sgr(0, 79, 23, False))
    log.close()


def test_buttons_x10():
    log = session(b"\033[?1000h")
    at(7, 3)
    for name, code in (("left", 0), ("middle", 1), ("right", 2)):
        mouse.send("down", name)
        expect(log, f"1000 X10: {name} press", x10(code, 7, 3))
        mouse.send("up", name)
        expect(log, f"1000 X10: {name} release is button 3", x10(3, 7, 3))
    log.close()


def test_drag():
    log = session(b"\033[?1002;1006h")
    at(5, 5)
    at(6, 6)
    expect(log, "1002: moving without a button reports nothing", b"")
    for name, code in (("left", 0), ("middle", 1), ("right", 2)):
        at(5, 5)
        mouse.send("down", name)
        log.take(sgr(code, 5, 5))
        at(6, 5)
        expect(log, f"1002: drag with {name}", sgr(code + 32, 6, 5))
        x, y, w, h = rect()
        cw = (w - 4) // 80
        mouse.send("move", x + 2 + 6 * cw + 1, y + 2 + 5 * ((h - 4) // 24) + 1)
        expect(log, f"1002: {name}, within the same cell nothing", b"")
        mouse.send("up", name)
        log.take()
    log.close()


def test_any_motion():
    log = session(b"\033[?1003;1006h")
    at(5, 5)
    log.take()
    at(6, 5)
    expect(log, "1003: motion without a button is button 3", sgr(35, 6, 5))
    at(6, 6)
    expect(log, "1003: and on to the next row", sgr(35, 6, 6))
    mouse.send("down", "left")
    expect(log, "1003: press", sgr(0, 6, 6))
    at(7, 6)
    expect(log, "1003: drag", sgr(32, 7, 6))
    mouse.send("up", "left")
    expect(log, "1003: release", sgr(0, 7, 6, False))
    log.close()
    log = session(b"\033[?1003h")
    at(5, 5)
    log.take()
    at(6, 5)
    expect(log, "1003 X10: motion", x10(35, 6, 5))
    log.close()


def test_wheel():
    log = session(b"\033[?1000;1006h")
    at(9, 4)
    mouse.send("wheel", -1)
    expect(log, "wheel up is 64", sgr(64, 9, 4))
    mouse.send("wheel", 1)
    expect(log, "wheel down is 65", sgr(65, 9, 4))
    for i in range(6):
        mouse.send("wheel", 1)
        time.sleep(0.1)
    expect(log, "six notches give six reports", sgr(65, 9, 4) * 6)
    log.close()
    log = session(b"\033[?1000h")
    at(9, 4)
    mouse.send("wheel", -1)
    expect(log, "X10: wheel up", x10(64, 9, 4))
    mouse.send("wheel", 1)
    expect(log, "X10: wheel down", x10(65, 9, 4))
    log.close()


def test_wheel_alternate_screen():
    log = session(b"\033[?1049h")
    at(9, 4)
    mouse.send("wheel", -1)
    expect(log, "alternate screen, no mouse mode: wheel up is three arrows", b"\033[A" * 3)
    mouse.send("wheel", 1)
    expect(log, "wheel down too", b"\033[B" * 3)
    log.close()
    log = session(b"\033[?1049h\033[?1h")
    at(9, 4)
    mouse.send("wheel", -1)
    expect(log, "with DECCKM in application form", b"\033OA" * 3)
    log.close()


def test_modifiers():
    log = session(b"\033[?1002;1006h")
    at(5, 5)
    for mods, value, name in ((("ctrl",), 16, "Ctrl"), (("alt",), 8, "Alt"), (("ctrl", "alt"), 24, "Ctrl+Alt")):
        for m in mods:
            mouse.send("hold", m)
        time.sleep(0.15)
        at(5, 5)
        log.take()
        mouse.send("down", "left")
        expect(log, f"{name}: press", sgr(value, 5, 5))
        at(6, 5)
        expect(log, f"{name}: drag", sgr(value + 32, 6, 5))
        mouse.send("up", "left")
        expect(log, f"{name}: release", sgr(value, 6, 5, False))
        mouse.send("wheel", -1)
        expect(log, f"{name}: wheel", sgr(value + 64, 6, 5))
        for m in mods:
            mouse.send("release", m)
        time.sleep(0.15)
    log.close()
    log = session(b"\033[?1000h")
    at(5, 5)
    mouse.send("hold", "ctrl")
    time.sleep(0.15)
    mouse.send("down", "right")
    expect(log, "X10 with Ctrl: press", x10(16 + 2, 5, 5))
    mouse.send("up", "right")
    expect(log, "X10 with Ctrl: release keeps the modifier", x10(16 + 3, 5, 5))
    mouse.send("release", "ctrl")
    time.sleep(0.15)
    log.close()


def test_off():
    log = session(b"\033[?1000;1006h\033[?1000l\033[?1006l")
    at(5, 5)
    mouse.send("click", "middle")
    time.sleep(0.2)
    mouse.send("wheel", -1)
    expect(log, "mode off: no reports", b"")
    log.close()


def test_x10_limit():
    # A small font makes the columns past what X10 can express fit on the output.
    cols, rows = 240, 5
    log = session(b"\033[?1000h", geometry=f"{cols}x{rows}", font="monospace:size=5")
    x, _, w, _ = rect()
    check(x + w <= 1000, f"the {cols} column window fits on the output", f"{w} pixels from {x}")
    at(222, 2, cols, rows)
    mouse.send("click", "left")
    expect(log, "X10: column 223 is the last it can express", x10(0, 222, 2) + x10(3, 222, 2))
    at(223, 2, cols, rows)
    mouse.send("click", "left")
    expect(log, "X10: past it nothing, rather than a wrong column", b"")
    log.close()
    log = session(b"\033[?1000;1006h", geometry=f"{cols}x{rows}", font="monospace:size=5")
    at(239, 4, cols, rows)
    mouse.send("click", "left")
    expect(log, "SGR has no such limit", sgr(0, 239, 4) + sgr(0, 239, 4, False))
    log.close()


def main():
    global mouse
    # Its virtual keyboard also keeps one on the seat, without one no window has focus.
    mouse = Mouse()
    test_buttons_sgr()
    test_edges()
    test_buttons_x10()
    test_drag()
    test_any_motion()
    test_wheel()
    test_wheel_alternate_screen()
    test_modifiers()
    test_off()
    test_x10_limit()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
