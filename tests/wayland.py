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


def committed_after_ack(log):
    """Whether the surface was committed after the last ack_configure."""
    lines = requests(log)
    acks = [i for i, line in enumerate(lines) if ".ack_configure(" in line]
    if not acks:
        return False
    return any(re.search(r"wl_surface#\d+\.commit\(", line) for line in lines[acks[-1]:])


def test_hidden(log):
    win = Window(env={"WAYLAND_DEBUG": "client"}, stderr=log, app_id="mwt")
    # on another workspace no frame callback comes: output leaves one pending
    swaymsg('[app_id="mwt"]', "move", "to", "workspace", "2")
    time.sleep(0.3)
    win.feed(b"text\r\n")
    time.sleep(0.3)
    swaymsg('[app_id="mwt"]', "resize", "set", "500", "300")
    end = time.time() + 2
    while time.time() < end and not committed_after_ack(log):
        time.sleep(0.05)
    check(committed_after_ack(log), "hidden: a configure is acked and committed")
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
    test_long_names()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
