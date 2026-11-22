#!/usr/bin/env python3
"""URLs: Ctrl+click opens one with the mwtrc's urlLauncher, a fake here that
writes what it got, with an activation token for the click; Open link in the menu
does the same; with Ctrl held the URL is underlined and the pointer a hand. Not
while a program has the mouse, unless Shift takes it back. Run with `make links`,
which goes through tests/headless.sh."""
import glob
import os
import re
import subprocess
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, combo, finish, rect, settle
from pixel import Grid

mouse = None
OUT = os.path.join(mwtlib.DIR, "opened")
URL = "https://example.org/wiki/A_(b)?c=d"
FG = 0xdcdccc


def at(col, row, cols=80, rows=24):
    x, y, w, h = rect()
    cw, ch = (w - 2 * mwtlib.PAD) // cols, (h - 2 * mwtlib.PAD) // rows
    mouse.send("move", x + mwtlib.PAD + col * cw + cw // 2, y + mwtlib.PAD + row * ch + ch // 2)
    time.sleep(0.1)


def opened(limit=3.0):
    """What the opener got since the last call: (token, url) for each start."""
    end = time.time() + limit
    while time.time() < end:
        files = glob.glob(os.path.join(OUT, "*"))
        if files:
            time.sleep(0.2)  # a second start would come with it
            break
        time.sleep(0.05)
    got = []
    for f in sorted(glob.glob(os.path.join(OUT, "*"))):
        with open(f) as fh:
            token, _, url = fh.read().rstrip("\n").partition(" ")
        got.append((token, url))
        os.remove(f)
    return got


def nothing_opened():
    return opened(limit=0.6) == []


def setup():
    os.makedirs(OUT, exist_ok=True)
    opener = os.path.join(mwtlib.DIR, "opener.sh")
    with open(opener, "w") as f:
        # written whole, then renamed: the test reads only what is complete
        f.write('#!/bin/sh\nt=$(mktemp "$0.XXXXXX")\n'
                'echo "${XDG_ACTIVATION_TOKEN:--} $1" >"$t"\n'
                f'mv "$t" "{OUT}/$(date +%s%N)"\n')
    os.chmod(opener, 0o755)
    d = os.path.join(os.environ["XDG_CONFIG_HOME"], "mwt")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "mwtrc"), "w") as f:
        f.write(f"Mwt*urlLauncher: /bin/sh {opener}\n")


def screen(win):
    """The URL in row 1 from column 4, one over rows 3 to 5 that wrap."""
    win.feed(b"\033[2J\033[H\r\nsee " + URL.encode() + b" and\r\n\r\n" +
             b"x" * 70 + b" http://wrapped.example.org/" + b"y" * 90 + b" z\033[12;1H")


def test_click(win):
    at(10, 1)
    mouse.send("click", "left")
    check(nothing_opened(), "a click without Ctrl opens nothing")
    mouse.send("hold", "ctrl")
    at(10, 1)
    mouse.send("click", "left")
    mouse.send("release", "ctrl")
    got = opened()
    check(len(got) == 1 and got[0][1] == URL, "Ctrl+click opens the URL, once", repr(got))
    check(got and got[0][0] != "-", "with an activation token", repr(got))
    for col, what in ((4, "its first cell"), (4 + len(URL) - 1, "its last cell")):
        mouse.send("hold", "ctrl")
        at(col, 1)
        mouse.send("click", "left")
        mouse.send("release", "ctrl")
        got = opened()
        check([u for _, u in got] == [URL], f"Ctrl+click on {what}", repr(got))
    for col, what in ((1, "the word before"), (4 + len(URL) + 1, "the word after")):
        mouse.send("hold", "ctrl")
        at(col, 1)
        mouse.send("click", "left")
        mouse.send("release", "ctrl")
        check(nothing_opened(), f"Ctrl+click on {what} opens nothing")
    # over three lines that wrap, from the middle one
    mouse.send("hold", "ctrl")
    at(20, 4)
    mouse.send("click", "left")
    mouse.send("release", "ctrl")
    got = opened()
    want = "http://wrapped.example.org/" + "y" * 90
    check([u for _, u in got] == [want], "Ctrl+click on a URL that wraps", repr(got))


def underlined(g, col, row):
    return bool(g.full_rows(col, row, FG))


def test_hover(win, log):
    """With the pointer shown: sway's hide_cursor of the harness ends in a leave,
    and the hover with it. It is drawn into the screenshot, so the cells near it
    are not looked at."""
    mwtlib.swaymsg("seat", "*", "hide_cursor", "0")
    at(30, 1)
    near = lambda c, r, pc, pr: pc - 3 <= c <= pc + 3 and pr <= r <= pr + 2
    g = Grid(settle(), 80, 24)
    check(not underlined(g, 12, 1), "without Ctrl no underline")
    mouse.send("hold", "ctrl")
    at(30, 1)
    g = Grid(settle(), 80, 24)
    cols = [c for c in range(4, 4 + len(URL)) if not near(c, 1, 30, 1)]
    under = [c for c in cols if underlined(g, c, 1)]
    check(under == cols, "with Ctrl the URL under the pointer is underlined",
          f"{len(under)} of {len(cols)} cells")
    check(not underlined(g, 2, 1) and not underlined(g, 4 + len(URL) + 1, 1),
          "and only the URL")
    at(40, 4)
    g = Grid(settle(), 80, 24)
    check(not underlined(g, 12, 1) and all(underlined(g, c, r) for c, r in ((71, 3), (0, 4), (79, 4), (0, 5))),
          "moved to the URL that wraps: it, over its lines, and not the first")
    mouse.send("release", "ctrl")
    g = Grid(settle(), 80, 24)
    check(not underlined(g, 0, 4), "Ctrl let go: no underline")
    mwtlib.swaymsg("seat", "*", "hide_cursor", "100")
    # the pointer: a hand with Ctrl over a URL, the text cursor again without
    surfaces = []
    for line in open(log, errors="replace"):
        if " -> " in line and (m := re.search(r"set_cursor\(\d+, wl_surface#(\d+),", line)):
            surfaces.append(m[1])
    hands = len(set(surfaces))
    if hands < 2:
        print("info: the cursor theme has no hand: the text cursor over links too")
    else:
        check(surfaces[-1] == surfaces[0], "the text cursor back without Ctrl", repr(surfaces))


def sizes(log):
    return [(int(m[1]), int(m[2])) for line in open(log, errors="replace")
            if " -> " in line and (m := re.search(r"xdg_positioner#\d+\.set_size\((\d+), (\d+)\)", line))]


def test_menu(win, log):
    at(10, 1)
    mouse.send("click", "right")
    time.sleep(0.3)
    combo("Down")
    combo("Return")
    got = opened()
    check([u for _, u in got] == [URL], "the menu on a URL: Open link opens it", repr(got))
    check(got and got[0][0] != "-", "with an activation token", repr(got))
    at(1, 1)
    mouse.send("click", "right")
    time.sleep(0.3)
    combo("Escape")
    s = sizes(log)
    check(len(s) >= 2 and s[-1][1] < s[-2][1], "off a URL the menu has no Open link", repr(s))
    check(nothing_opened(), "and opens nothing")


def test_mouse_mode(win):
    win.feed(b"\033[?1000;1006h")
    time.sleep(0.2)
    mouse.send("hold", "ctrl")
    at(10, 1)
    mouse.send("click", "left")
    mouse.send("release", "ctrl")
    check(nothing_opened(), "a program has the mouse: Ctrl+click is its")
    mouse.send("hold", "ctrl")
    mouse.send("hold", "shift")
    at(10, 1)
    mouse.send("click", "left")
    mouse.send("release", "shift")
    mouse.send("release", "ctrl")
    got = opened()
    check([u for _, u in got] == [URL], "Shift+Ctrl+click takes it back and opens", repr(got))
    win.feed(b"\033[?1000;1006l")


def focused(app_id):
    n = mwtlib.node(app_id)
    return bool(n and n.get("focused"))


def wait(cond, limit=3.0):
    end = time.time() + limit
    while time.time() < end and not cond():
        time.sleep(0.05)
    return cond()


def test_activation(win):
    """The token takes the focus: mwc's xdgc stands in for the browser and asks
    for it with what the opener got, as Firefox does with XDG_ACTIVATION_TOKEN.
    sway only marks a window urgent by default; with focus_on_window_activation
    focus it gives the focus for a token with the serial of an input event, and
    still not for one without, which is checked first."""
    xdgc = os.environ.get("XDGC")
    if not xdgc:
        print("info: no XDGC (mwc's build/xdgc): the token's focus not checked")
        return
    fifo = os.path.join(mwtlib.DIR, "xdgc.fifo")
    os.mkfifo(fifo)
    p = subprocess.Popen([xdgc, "app=browser", "size=200x100", f"control={fifo}"],
                         stdout=subprocess.PIPE, text=True)
    ctl = open(fifo, "w", buffering=1)
    check(wait(lambda: rect("browser")), "the browser stand-in maps")
    at(60, 10)  # focus follows the mouse back to mwt
    mouse.send("click", "left")
    check(wait(lambda: focused("mwt")), "mwt has the focus again")
    mwtlib.swaymsg("focus_on_window_activation", "focus")
    token = os.path.join(mwtlib.DIR, "token")
    ctl.write(f"token_noinput {token}\n")
    check(wait(lambda: os.path.exists(token) and os.path.getsize(token) > 0), "a token without input")
    ctl.write(f"activate {token}\n")
    check(wait(lambda: mwtlib.urgent("browser")), "without a serial: urgent")
    check(focused("mwt"), "and mwt keeps the focus")
    os.remove(token)
    mouse.send("hold", "ctrl")
    at(10, 1)
    mouse.send("click", "left")
    mouse.send("release", "ctrl")
    got = opened()
    with open(token, "w") as f:
        f.write(got[0][0] if got else "-")
    ctl.write(f"activate {token}\n")
    check(wait(lambda: focused("browser")), "the browser takes the focus with the token",
          f"mwt focused: {focused('mwt')}")
    mwtlib.swaymsg("focus_on_window_activation", "urgent")
    ctl.write("quit\n")
    ctl.close()
    p.wait(5)
    wait(lambda: not rect("browser"))


def main():
    global mouse
    setup()
    mouse = Mouse()
    log = os.path.join(mwtlib.DIR, "links-wayland.log")
    win = Window(env={"WAYLAND_DEBUG": "1"}, stderr=log)
    screen(win)
    settle()
    test_click(win)
    test_hover(win, log)
    test_menu(win, log)
    screen(win)
    test_mouse_mode(win)
    test_activation(win)
    win.close()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
