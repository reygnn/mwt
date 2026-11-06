#!/usr/bin/env python3
"""LeakSanitizer over all that takes and gives back memory: scrollback, resizes,
zoom, scale, the menu, the clipboard. mwt then ends the normal way, with its
program. Needs an ASan build; `make evil` has one and turned leak detection off
for everything else, because a test there kills mwt rather than ending it. Run
through tests/headless.sh."""
import os
import re
import subprocess
import sys
import time

import mwtlib
from mwtlib import LOG, Mouse, Window, check, combo, finish, rect, swaymsg


def wl_copy(text):
    subprocess.run(["wl-copy"], input=text, check=True)
    time.sleep(0.2)


def wl_paste():
    subprocess.run(["timeout", "3", "wl-paste", "-n"], capture_output=True)
    subprocess.run(["timeout", "3", "wl-paste", "-t", "STRING"], capture_output=True)


def exercise(win, mouse):
    lines = b"".join(b"\033[3%dmzeile %d \xc3\xa4\xe6\xbc\xa2\033[0m\r\n" % (i % 8, i) for i in range(6000))
    win.feed(lines)
    time.sleep(1)
    win.feed(b"\033]0;titel\a\033[?1049handers\033[?1049l\033[3J\033c")
    win.feed(lines[:40000])
    time.sleep(0.5)
    for w, h in ((500, 300), (900, 600), (120, 80), (700, 500)):
        swaymsg('[app_id="mwt"]', "resize", "set", str(w), str(h))
        time.sleep(0.2)
    for key in ("plus", "plus", "minus", "0"):
        combo(key, "ctrl", "shift")
    swaymsg("output", "HEADLESS-1", "scale", "2")
    time.sleep(0.5)
    swaymsg("output", "HEADLESS-1", "scale", "1")
    time.sleep(0.5)
    x, y, w, h = rect()
    for _ in range(3):
        mouse.send("move", x + 40, y + 40)
        mouse.send("click", "right")
        time.sleep(0.3)
        mouse.send("move", x + w // 2, y + h - 10)
        mouse.send("click", "left")
        time.sleep(0.3)
    for i in range(4):
        combo("a", "ctrl", "shift")
        combo("c", "ctrl", "shift")
        time.sleep(0.2)
        wl_paste()
        wl_copy(b"von aussen %d \xc3\xa4\n" % i)
        combo("v", "ctrl", "shift")
        mouse.send("click", "middle")
        time.sleep(0.2)


def main():
    if b"asan" not in subprocess.run(["ldd", mwtlib.MWT], capture_output=True).stdout:
        print("info: skipped, this needs an ASan build")
        return 0
    os.environ["ASAN_OPTIONS"] = "detect_leaks=1"
    # What fontconfig keeps for good on its first use: once, and not more with use.
    supp = os.path.join(mwtlib.DIR, "lsan.supp")
    with open(supp, "w") as f:
        f.write("leak:libfontconfig\nleak:libexpat\n")
    os.environ["LSAN_OPTIONS"] = f"suppressions={supp}:print_suppressions=0"
    mouse = Mouse()
    win = Window()
    exercise(win, mouse)
    win.out.close()
    win.out = None
    try:
        rc = win.proc.wait(15)
    except subprocess.TimeoutExpired:
        win.proc.kill()
        rc = "still running"
    mouse.close()
    with open(LOG, errors="replace") as f:
        log = f.read()
    m = re.search(r"LeakSanitizer", log)
    if "LeakSanitizer has encountered a fatal error" in log:
        print("FAIL: LeakSanitizer could not run here")
        mwtlib.fails += 1
    elif m:
        summary = re.search(r"SUMMARY: AddressSanitizer: .*", log)
        print(log[max(0, m.start() - 20):m.start() + 6000])
        check(False, "no leaks", summary.group(0) if summary else "")
    else:
        check(True, "no leaks after scrollback, resizes, zoom, scale, menu and clipboard")
    check(rc == 0, "mwt ends with its program", str(rc))
    return finish()


if __name__ == "__main__":
    sys.exit(main())
