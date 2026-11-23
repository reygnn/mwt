#!/usr/bin/env python3
"""What mwt asks of the compositor and answers it, from its WAYLAND_DEBUG log and
sway's tree, where windows with the app_id mwt float: a configure on a hidden
window is committed, the min size, -T and -a cut to one message, the token mwt
is started with. sway sends no
configure_bounds: test_util has fit_bounds. Run with
`make wayland`, which goes through tests/headless.sh."""
import os
import re
import subprocess
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
    """In a flood a frame callback is drawn and sent at once: a turn of reading
    the pty took up to 16 ms of the frame first. In sway's log, which has the
    protocol (SWAY_WAYLAND_DEBUG, make wayland), from each frame's done sent to
    mwt's next commit received; mwt's own log shows requests queued, not sent."""
    slog = os.path.join(mwtlib.DIR, "sway.log")
    if not os.environ.get("SWAY_WAYLAND_DEBUG"):
        print("info: yes: frame callbacks not measured, sway logs no protocol")
        return
    start = os.path.getsize(slog)
    win = Window("yes", env={"WAYLAND_DEBUG": "client"}, stderr=log)
    time.sleep(1.5)
    sid = main_surface(requests(log))
    win.close()
    with open(slog, errors="replace") as f:
        f.seek(start)
        lines = f.readlines()
    stamp = lambda line: float(re.match(r"\[\s*(\d+\.\d+)\]", line).group(1))
    frames, waits, done = set(), [], None
    for line in lines:
        if m := re.search(rf"wl_surface#{sid}\.frame\(new id wl_callback#(\d+)\)", line):
            frames.add(m.group(1))
        elif (m := re.search(r" -> wl_callback#(\d+)\.done\(", line)) and m.group(1) in frames:
            done = stamp(line)
        elif done is not None and f"wl_surface#{sid}.commit(" in line and " -> " not in line:
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


def focused(app_id):
    n = node(app_id)
    return bool(n and n.get("focused"))


def wait(cond, limit=3.0):
    end = time.time() + limit
    while time.time() < end and not cond():
        time.sleep(0.05)
    return cond()


def test_startup_token(mouse):
    """The token mwt is started with: its window asks for the focus with it, and
    the shell does not see it. mwc's xdgc stands in for the launcher and makes
    a token for a click on it. no_focus keeps sway from focusing the new window
    by itself, so a window without the token stays unfocused, which is checked
    first; focus_on_window_activation focus gives it the focus with one."""
    xdgc = os.environ.get("XDGC")
    if not xdgc or not os.access(xdgc, os.X_OK):
        print("info: no XDGC (mwc's build/xdgc): the startup token not checked")
        return
    fifo = os.path.join(mwtlib.DIR, "launcher.fifo")
    os.mkfifo(fifo)
    p = subprocess.Popen([xdgc, "app=launcher", "size=200x100", f"control={fifo}"],
                         stdout=subprocess.DEVNULL)
    fd = -1
    end = time.time() + 5
    while fd < 0 and p.poll() is None and time.time() < end:
        try:
            fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.05)
    if not check(fd >= 0, "the launcher stand-in reads its fifo", f"exit {p.poll()}"):
        p.kill()
        return
    os.set_blocking(fd, True)
    ctl = os.fdopen(fd, "w", buffering=1)
    swaymsg("no_focus", '[app_id="mwt-token"]')
    swaymsg("focus_on_window_activation", "focus")
    envfile = os.path.join(os.environ["HOME"], "token-env")

    def start(token):
        """mwt with token, whose shell writes what it got of it to envfile"""
        if os.path.exists(envfile):
            os.remove(envfile)
        r = wait(lambda: rect("launcher"))
        mouse.send("move", r[0] + 20, r[1] + 20)
        mouse.send("click", "left")
        wait(lambda: focused("launcher"))
        env = {"XDG_ACTIVATION_TOKEN": token, "DESKTOP_STARTUP_ID": token} if token else None
        return Window("sh", "-c", f"env | grep -E 'ACTIVATION|STARTUP' >'{envfile}'; "
                      "echo done >>'{envfile}'; exec sleep 100".replace("{envfile}", envfile),
                      app_id="mwt-token", env=env)

    if not check(wait(lambda: rect("launcher")), "the launcher stand-in maps"):
        p.kill()
        return
    win = start(None)
    time.sleep(0.5)
    check(not focused("mwt-token") and focused("launcher"),
          "without a token the new window stays unfocused (no_focus)")
    win.close()
    token = os.path.join(mwtlib.DIR, "token")
    ctl.write(f"token {token}\n")
    got = wait(lambda: os.path.exists(token) and os.path.getsize(token) > 0)
    if check(got, "the launcher has a token for its click"):
        win = start(open(token).read().strip())
        check(wait(lambda: focused("mwt-token")), "with it mwt takes the focus")
        check(wait(lambda: os.path.exists(envfile) and "done" in open(envfile).read()),
              "the shell starts")
        seen = open(envfile).read() if os.path.exists(envfile) else ""
        check(seen.strip() == "done", "and has neither XDG_ACTIVATION_TOKEN nor DESKTOP_STARTUP_ID",
              seen)
        win.close()
    swaymsg("focus_on_window_activation", "urgent")
    ctl.write("quit\n")
    ctl.close()
    p.wait(5)
    wait(lambda: not rect("launcher"))


def main():
    mouse = Mouse()
    test_hidden(os.path.join(mwtlib.DIR, "hidden.log"))
    test_min_size(os.path.join(mwtlib.DIR, "minsize.log"))
    test_click(os.path.join(mwtlib.DIR, "click.log"), mouse)
    test_title(os.path.join(mwtlib.DIR, "title.log"))
    test_flood_frames(os.path.join(mwtlib.DIR, "flood.log"))
    test_long_names()
    test_startup_token(mouse)
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
