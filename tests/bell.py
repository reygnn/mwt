#!/usr/bin/env python3
"""BEL: with the focus elsewhere mwt asks for attention through xdg-activation,
which sway shows as urgent; focused, nothing. Once until the focus comes back,
also for a program ringing without end. Run with `make bell`, which goes through
tests/headless.sh."""
import os
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, finish, urgent


def wait_urgent(want, limit=3.0):
    end = time.time() + limit
    while time.time() < end:
        if urgent() == want:
            return True
        time.sleep(0.05)
    return urgent() == want


def tokens(log):
    """Activation tokens mwt asked for, from its WAYLAND_DEBUG log."""
    with open(log) as f:
        return sum(1 for line in f if " -> " in line and ".get_activation_token(" in line)


def main():
    # Its virtual keyboard keeps one on the seat. Without one no window has focus.
    mouse = Mouse()
    log = os.path.join(mwtlib.DIR, "bell-wayland.log")
    win = Window(env={"WAYLAND_DEBUG": "1"}, stderr=log)

    win.feed(b"\a")
    time.sleep(0.5)
    check(urgent() is False, "focused: BEL leaves the window as it is", repr(urgent()))

    other = Window("sleep", "100", app_id="other", geometry="10x2")
    time.sleep(0.3)
    win.feed(b"\a")
    check(wait_urgent(True), "not focused: BEL marks the window urgent", repr(urgent()))
    other.close()
    check(wait_urgent(False), "focus back: no longer urgent", repr(urgent()))

    # ringing without end: one request for the whole time without focus
    other = Window("sleep", "100", app_id="other", geometry="10x2")
    time.sleep(0.3)
    before = tokens(log)
    for _ in range(20):
        win.feed(b"\a" * 500)
        time.sleep(0.02)
    check(wait_urgent(True), "10000 BELs: urgent", repr(urgent()))
    time.sleep(0.5)
    n = tokens(log) - before
    check(n == 1, "10000 BELs: one activation token asked for", f"{n} tokens")
    other.close()
    check(wait_urgent(False), "focus back after the flood: no longer urgent", repr(urgent()))
    win.close()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
