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


mouse_xy = (0, 0)


def at(col, row, cols=80, rows=24):
    global mouse_xy
    x, y, w, h = rect()
    cw, ch = (w - 2 * mwtlib.PAD) // cols, (h - 2 * mwtlib.PAD) // rows
    mouse_xy = (x + mwtlib.PAD + col * cw + cw // 2, y + mwtlib.PAD + row * ch + ch // 2)
    mouse.send("move", *mouse_xy)
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


def test_lost_release(win):
    """The release of a Ctrl+click that went to the menu: the next click's
    release is a selection's again, which copies it to the primary selection."""
    mouse.send("hold", "ctrl")
    at(10, 1)
    mouse.send("down", "left")
    mouse.send("release", "ctrl")
    mouse.send("down", "right")
    time.sleep(0.3)
    mouse.send("up", "left")
    mouse.send("up", "right")
    combo("Escape")
    opened()
    at(0, 1)
    mouse.send("down", "left")
    at(2, 1)
    mouse.send("up", "left")
    paste = lambda: subprocess.run(["wl-paste", "-p", "-n"], capture_output=True, text=True,
                                   timeout=5).stdout
    wait(lambda: paste() == "see")
    got = paste()
    check(got == "see", "after a Ctrl+click whose release the menu took, a drag selects", repr(got))


def test_zoom(win):
    """A zoom with the pointer still: the URL under it now is the one opened.
    With the pointer shown, or sway's hide_cursor sends a leave, and the enter
    of the click would work the cell out anew without the fix."""
    mwtlib.swaymsg("seat", "*", "hide_cursor", "0")
    win.feed(b"\033[2J\033[H" + b"".join(b"http://row%d.example/\r\n" % r for r in range(20)))
    settle()
    at(5, 12)
    for _ in range(3):
        combo("plus", "ctrl", "shift")
    settle()
    x, y, w, h = rect()
    ch = (h - 2 * mwtlib.PAD) // 24
    _, py = mouse_xy
    row = (py - y - mwtlib.PAD) // ch
    mouse.send("hold", "ctrl")
    mouse.send("click", "left")
    mouse.send("release", "ctrl")
    got = opened()
    check(row != 12 and [u for _, u in got] == [f"http://row{row}.example/"],
          "after a zoom Ctrl+click opens the URL now under the pointer", f"row {row}: {got!r}")
    combo("0", "ctrl", "shift")
    settle()
    mwtlib.swaymsg("seat", "*", "hide_cursor", "100")


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
    # the image has no cursor theme: libwayland-cursor's own cursors have a hand
    check(len(set(surfaces)) == 2, "a hand over the URL with Ctrl", repr(surfaces))
    check(surfaces[-1] == surfaces[0], "the text cursor back without Ctrl", repr(surfaces))


def sizes(log):
    return [(int(m[1]), int(m[2])) for line in open(log, errors="replace")
            if " -> " in line and (m := re.search(r"xdg_positioner#\d+\.set_size\((\d+), (\d+)\)", line))]


def menu_up(log, before):
    """Waits until the menu asked after before asks for its size."""
    check(wait(lambda: len(sizes(log)) > before), "the menu opens")


def test_menu(win, log):
    at(10, 1)
    before = len(sizes(log))
    mouse.send("click", "right")
    menu_up(log, before)
    time.sleep(0.1)  # its configure: the keys go to it once it is mapped
    combo("Down")
    combo("Return")
    got = opened()
    check([u for _, u in got] == [URL], "the menu on a URL: Open link opens it", repr(got))
    check(got and got[0][0] != "-", "with an activation token", repr(got))
    at(1, 1)
    before = len(sizes(log))
    mouse.send("click", "right")
    menu_up(log, before)
    time.sleep(0.1)
    combo("Escape")
    s = sizes(log)
    check(len(s) >= 2 and s[-1][1] < s[-2][1], "off a URL the menu has no Open link", repr(s))
    check(nothing_opened(), "and opens nothing")


def test_mouse_mode(win):
    # with a mark after it, drawn once mwt has read what comes before
    win.feed(b"\033[?1000;1006h\033[12;1HM")
    settle()
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


def test_many_words():
    """A urlLauncher of 31 words: nothing starts, and stderr says why. In the
    mwt window sway floats, with the mwtrc changed for it and put back."""
    rc = os.path.join(os.environ["XDG_CONFIG_HOME"], "mwt", "mwtrc")
    keep = open(rc).read()
    with open(rc, "w") as f:
        f.write("Mwt*urlLauncher: " + " ".join(["w"] * 31) + "\n")
    err = os.path.join(mwtlib.DIR, "many.log")
    try:
        with Window(stderr=err) as w:
            w.feed(b"\033[2J\033[H\r\nsee " + URL.encode() + b"\r\n")
            settle()
            mouse.send("hold", "ctrl")
            at(10, 1)
            mouse.send("click", "left")
            mouse.send("release", "ctrl")
            check(nothing_opened(), "a urlLauncher of 31 words starts nothing")
            check(wait(lambda: "more than 30 words" in open(err, errors="replace").read()),
                  "and says so on stderr", open(err, errors="replace").read()[-300:])
    finally:
        with open(rc, "w") as f:
            f.write(keep)


TOAST_BG, TOAST_BORDER, TOAST_FG = 0x2b2b2b, 0x808080, 0xffffff
TOAST_MARGIN, TOAST_PAD = 16, 12  # util.h


def toast_edges(img):
    """The toast's border on the row TOAST_MARGIN: (left, right), or None."""
    xs = [x for x in range(img.w) if img.px(x, TOAST_MARGIN) == TOAST_BORDER]
    return (xs[0], xs[-1]) if xs else None


def test_missing_opener():
    """A urlLauncher that is not there: a toast at the top of the window, centered,
    says so for three seconds, and stderr too. Open link in the menu first, with
    the keys: nothing else in the window changes then, so the toast must show by
    itself. A Ctrl+click would not tell: sway's hide_cursor (100 ms here) takes
    the pointer away and gives it back, and the underline drawn anew each time
    commits the window too. Then Ctrl+click, which shows it again."""
    rc = os.path.join(os.environ["XDG_CONFIG_HOME"], "mwt", "mwtrc")
    keep = open(rc).read()
    with open(rc, "w") as f:
        f.write("Mwt*urlLauncher: mwt-no-such-opener --tab\n")
    err = os.path.join(mwtlib.DIR, "missing.log")
    try:
        with Window(stderr=err, env={"WAYLAND_DEBUG": "1"}) as w:
            w.feed(b"\033[2J\033[H\r\nsee " + URL.encode() + b"\r\n")
            at(10, 1)
            before = settle()
            check(toast_edges(before) is None, "no toast before")
            n = len(sizes(err))
            mouse.send("click", "right")
            menu_up(err, n)
            time.sleep(0.1)
            combo("Down")
            combo("Return")
            t0 = time.time()
            check(wait(lambda: toast_edges(mwtlib.shot())), "Open link: a toast shows")
            img = settle()
            edges = toast_edges(img)
            if check(edges, "and stays", f"row {TOAST_MARGIN}: {img.px(img.w // 2, TOAST_MARGIN):06x}"):
                left, right = edges
                check(abs(left + right - (img.w - 1)) <= 1, "centered", f"{left}..{right} of {img.w}")
                inside = [img.px(x, y) for y in range(TOAST_MARGIN + 1, TOAST_MARGIN + 2 * TOAST_PAD)
                          for x in range(left + 1, right)]
                check(inside.count(TOAST_BG) > len(inside) // 2 and TOAST_FG in inside,
                      "white text on dark grey")
                # "mwt-no-such-opener: not found" in cells of the window's font
                cw = (img.w - 2 * mwtlib.PAD) // 80
                n = len("mwt-no-such-opener: not found")
                check(abs((right - left + 1) - (n * cw + 2 * TOAST_PAD)) <= 2,
                      "as wide as its text", f"{right - left + 1} for {n} cells of {cw}")
            check(wait(lambda: "mwt-no-such-opener: No such file" in open(err, errors="replace").read()),
                  "stderr says why")
            check(wait(lambda: toast_edges(mwtlib.shot()) is None, 6.0), "the toast goes")
            gone = time.time() - t0
            check(2.5 <= gone <= 4.5, "after three seconds", f"{gone:.1f} s")
            check(settle() == before, "and leaves the window as it was")

            # a second one while the first shows starts the time anew
            def by_menu():
                at(10, 1)
                n = len(sizes(err))
                mouse.send("click", "right")
                menu_up(err, n)
                time.sleep(0.1)
                combo("Down")
                combo("Return")
                return time.time()
            t1 = by_menu()
            wait(lambda: toast_edges(mwtlib.shot()))
            time.sleep(max(0.0, t1 + 1.5 - time.time()))
            t2 = by_menu()
            time.sleep(max(0.0, t1 + 3.6 - time.time()))
            check(toast_edges(mwtlib.shot()), "a second toast keeps it past the first's time")
            check(wait(lambda: toast_edges(mwtlib.shot()) is None, 4.0), "and then it goes")
            gone = time.time() - t2
            check(2.5 <= gone <= 4.5, "three seconds after the second", f"{gone:.1f} s")

            # the window changes under it: centered anew, cut from the left,
            # off while too small, back when large again
            x, y, ww, hh = rect()
            mouse.send("hold", "ctrl")
            at(10, 1)
            mouse.send("click", "left")
            mouse.send("release", "ctrl")
            t3 = time.time()
            check(wait(lambda: toast_edges(mwtlib.shot())), "Ctrl+click: the toast again")
            mwtlib.swaymsg('[app_id="mwt"]', "resize", "set", "250", str(hh))
            wait(lambda: rect()[2] < ww)  # cells whole: a few pixels less
            img = settle()
            edges = toast_edges(img)
            cw = (ww - 2 * mwtlib.PAD) // 80
            room = (img.w - 2 * TOAST_MARGIN - 2 * TOAST_PAD) // cw
            if check(edges and img.w <= 250, "narrower: the toast stays", f"{edges} in {img.w}"):
                left, right = edges
                check(abs(left + right - (img.w - 1)) <= 1, "centered in it", f"{left}..{right}")
                check(abs((right - left + 1) - (room * cw + 2 * TOAST_PAD)) <= 2,
                      "cut to what fits", f"{right - left + 1} for {room} cells of {cw}")
            # lower than the toast with its margin; as narrow as sway lets it
            # be, where one cell, an ellipsis, would still fit
            mwtlib.swaymsg('[app_id="mwt"]', "resize", "set", "40", "30")
            small = wait(lambda: rect()[3] < 50)  # one row, its padding
            img = settle()
            check(small and toast_edges(img) is None, "too small: none", f"{rect()}")
            mwtlib.swaymsg('[app_id="mwt"]', "resize", "set", str(ww), str(hh))
            wait(lambda: rect()[2] == ww)
            check(time.time() - t3 < 2.8, "(all within its time)", f"{time.time() - t3:.1f} s")
            check(toast_edges(settle()), "large again: back")
            check(wait(lambda: toast_edges(mwtlib.shot()) is None, 4.0), "and goes in time")
            check(w.proc.poll() is None, "mwt runs on")
    finally:
        with open(rc, "w") as f:
            f.write(keep)


def test_activation(win, log):
    """The token takes the focus: mwc's xdgc stands in for the browser and asks
    for it with what the opener got, as Firefox does with XDG_ACTIVATION_TOKEN.
    sway only marks a window urgent by default; with focus_on_window_activation
    focus it gives the focus for a token with the serial of an input event, and
    still not for one without, which is checked first."""
    xdgc = os.environ.get("XDGC")
    if not xdgc or not os.access(xdgc, os.X_OK):
        print("info: no XDGC (mwc's build/xdgc): the token's focus not checked")
        return
    fifo = os.path.join(mwtlib.DIR, "xdgc.fifo")
    os.mkfifo(fifo)
    p = subprocess.Popen([xdgc, "app=browser", "size=200x100", f"control={fifo}"],
                         stdout=subprocess.PIPE, text=True)
    # it opens the fifo once it is up; one that died never would
    fd = -1
    end = time.time() + 5
    while fd < 0 and p.poll() is None and time.time() < end:
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.05)
    if not check(fd >= 0, "the browser stand-in reads its fifo", f"exit {p.poll()}"):
        p.kill()
        return
    os.set_blocking(fd, True)
    ctl = os.fdopen(fd, "w", buffering=1)
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
    # and the token of Open link, made for the key that chose it
    at(60, 10)
    mouse.send("click", "left")
    check(wait(lambda: focused("mwt")), "mwt has the focus again")
    at(10, 1)
    before = len(sizes(log))
    mouse.send("click", "right")
    menu_up(log, before)
    time.sleep(0.1)
    combo("Down")
    combo("Return")
    got = opened()
    with open(token, "w") as f:
        f.write(got[0][0] if got else "-")
    ctl.write(f"activate {token}\n")
    check(wait(lambda: focused("browser")), "the browser takes the focus with Open link's token",
          f"{got!r}, mwt focused: {focused('mwt')}")
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
    screen(win)
    settle()
    test_lost_release(win)
    test_zoom(win)
    win.close()
    test_many_words()
    test_missing_opener()
    win = Window(env={"WAYLAND_DEBUG": "1"}, stderr=log + ".2")
    screen(win)
    settle()
    test_activation(win, log + ".2")
    win.close()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
