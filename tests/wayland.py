#!/usr/bin/env python3
"""What mwt asks of the compositor and answers it, from its WAYLAND_DEBUG log and
sway's tree, where windows with the app_id mwt float: a configure on a hidden
window is committed, the min size, -T and -a cut to one message. sway sends no
configure_bounds: test_util has fit_bounds. Run with
`make wayland`, which goes through tests/headless.sh."""
import os
import re
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, finish, node, rect, swaymsg


def requests(log):
    with open(log) as f:
        return [line for line in f if " -> " in line]


def acks(log):
    return sum(".ack_configure(" in line for line in requests(log))


def committed_after_ack(log, before):
    """Whether a configure after the first before was acked, and the surface
    committed after the last ack."""
    lines = requests(log)
    at = [i for i, line in enumerate(lines) if ".ack_configure(" in line]
    if len(at) <= before:
        return False
    return any(re.search(r"wl_surface#\d+\.commit\(", line) for line in lines[at[-1]:])


def test_hidden(log):
    win = Window(env={"WAYLAND_DEBUG": "client"}, stderr=log, app_id="mwt")
    # on another workspace no frame callback comes: output leaves one pending
    swaymsg('[app_id="mwt"]', "move", "to", "workspace", "2")
    time.sleep(0.3)
    win.feed(b"text\r\n")
    time.sleep(0.3)
    before = acks(log)
    swaymsg('[app_id="mwt"]', "resize", "set", "500", "300")
    end = time.time() + 2
    while time.time() < end and not committed_after_ack(log, before):
        time.sleep(0.05)
    check(committed_after_ack(log, before), "hidden: a configure is acked and committed",
          f"{acks(log) - before} new acks")
    win.close()
    swaymsg("workspace", "1")


def test_min_size(log):
    win = Window(env={"WAYLAND_DEBUG": "client"}, stderr=log)
    time.sleep(0.2)
    sizes = [tuple(map(int, m.groups())) for line in requests(log)
             if (m := re.search(r"set_min_size\((\d+), (\d+)\)", line))]
    w, h = rect()[2:]
    cw, ch = (w - 2 * mwtlib.PAD) // 80, (h - 2 * mwtlib.PAD) // 24
    want = (cw + 2 * mwtlib.PAD, ch + 2 * mwtlib.PAD)
    check(sizes and sizes[-1] == want, "min size: one cell and the padding", f"{sizes}, want {want}")
    win.close()


def main_surface(lines):
    for line in lines:
        if m := re.search(r"get_xdg_surface\(new id xdg_surface#\d+, wl_surface#(\d+)\)", line):
            return m.group(1)
    return None


def surface_requests(log, since, what):
    lines = requests(log)
    sid = main_surface(lines)
    return [line for line in lines[since:] if f"wl_surface#{sid}.{what}(" in line]


def damages(log, since):
    return [tuple(map(int, m.groups())) for line in surface_requests(log, since, "damage_buffer")
            if (m := re.search(r"damage_buffer\((-?\d+), (-?\d+), (\d+), (\d+)\)", line))]


def test_click(log, mouse):
    """A double click and a click away from it draw the word's row anew, not the
    window: a click marked every row."""
    win = Window(env={"WAYLAND_DEBUG": "client"}, stderr=log)
    win.feed(b"word " * 15 + b"\r\n" * 2 + (b"text " * 15 + b"\r\n") * 20)
    time.sleep(0.5)
    x, y, w, h = rect()
    ch = (h - 2 * mwtlib.PAD) // 24
    cw = (w - 2 * mwtlib.PAD) // 80
    since = len(requests(log))
    mouse.send("move", x + mwtlib.PAD + 2 * cw, y + mwtlib.PAD + ch // 2)
    time.sleep(0.05)
    mouse.send("click", "left")
    mouse.send("click", "left")
    time.sleep(0.3)
    double = damages(log, since)
    since = len(requests(log))
    time.sleep(0.6) # past double_click_ms
    mouse.send("move", x + mwtlib.PAD + 10 * cw, y + mwtlib.PAD + 10 * ch + ch // 2)
    time.sleep(0.05)
    mouse.send("click", "left")
    time.sleep(0.3)
    away = damages(log, since)
    check(double and all(d[3] <= ch for d in double), "a double click damages its row",
          f"{double}, a row {ch}")
    check(away and all(d[3] <= ch for d in away), "a click away clears it, damaging that row",
          f"{away}, a row {ch}")
    win.close()


def test_title(log):
    """Titles alone change nothing in the window: no commit."""
    win = Window(env={"WAYLAND_DEBUG": "client"}, stderr=log)
    win.feed(b"text\r\n")
    time.sleep(0.3)
    since = len(requests(log))
    for i in range(30):
        win.feed(b"\033]2;title %d\a" % i)
        time.sleep(0.03)
    time.sleep(0.3)
    commits = surface_requests(log, since, "commit")
    check(len(commits) == 0, "30 titles: no commit", f"{len(commits)} commits")
    win.close()


def test_flood_frames(log):
    """In a flood a frame callback is drawn at once: a turn of reading the pty
    took up to 16 ms of the frame first. From each done to the next commit."""
    win = Window("yes", env={"WAYLAND_DEBUG": "client"}, stderr=log)
    time.sleep(1.5)
    win.close()
    with open(log) as f:
        lines = f.readlines()
    sid = main_surface([line for line in lines if " -> " in line])
    stamp = lambda line: float(re.match(r"\[\s*(\d+\.\d+)\]", line).group(1))
    waits, done = [], None
    for line in lines:
        if not line.startswith("["):
            continue
        if " -> " not in line and re.search(r"wl_callback#\d+\.done\(", line):
            done = stamp(line)
        elif done is not None and f"wl_surface#{sid}.commit(" in line:
            waits.append(stamp(line) - done)
            done = None
    waits.sort()
    median = waits[len(waits) // 2] if waits else 99
    check(len(waits) > 20 and median < 6,
          f"yes: a frame callback is drawn within 6 ms, median {median:.1f}",
          f"{len(waits)} frames")


def test_long_names():
    title = "t" * 70000
    app_id = "a" * 70000
    win = Window("sleep", "100", app_id=app_id, opts=("-T", title))
    n = node(app_id[:1023])
    check(win.proc.poll() is None and n is not None, "a 70000 byte -a and -T: mwt runs")
    if n:
        check(n.get("name") == title[:1023], "and the title is cut to 1023 bytes",
              str(len(n.get("name") or "")))
    win.close()


def main():
    mouse = Mouse()
    test_hidden(os.path.join(mwtlib.DIR, "hidden.log"))
    test_min_size(os.path.join(mwtlib.DIR, "minsize.log"))
    test_click(os.path.join(mwtlib.DIR, "click.log"), mouse)
    test_title(os.path.join(mwtlib.DIR, "title.log"))
    test_flood_frames(os.path.join(mwtlib.DIR, "flood.log"))
    test_long_names()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
