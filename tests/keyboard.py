#!/usr/bin/env python3
"""Dead keys, compose and key repeat, byte for byte, as in tests/keys.py. The
compose table is this test's own, so it does not depend on the system's locale.
Run with `make keyboard`, which goes through tests/headless.sh."""
import os
import signal
import subprocess
import sys
import time

import mwtlib
from mwtlib import Mouse, Window, check, combo, finish, keys
from mouse import expect, session

COMPOSE = """
<dead_acute> <a> : "á"
<dead_acute> <A> : "Á"
<dead_diaeresis> <a> : "ä"
<Multi_key> <a> <e> : "æ"
<Multi_key> <o> <c> : "©"
"""


def test_compose():
    log = session(b"")
    combo("dead_acute")
    combo("a")
    expect(log, "dead key, then a letter, in two calls", "á".encode())
    keys("-k", "dead_diaeresis", "-k", "a")
    expect(log, "dead key, then a letter, in one call", "ä".encode())
    combo("dead_acute")
    keys("A")  # as text: wtype -k takes names without regard to case
    expect(log, "a shifted letter after a dead key", "Á".encode())
    keys("-k", "Multi_key", "-k", "a", "-k", "e")
    expect(log, "Multi_key with two letters", "æ".encode())
    combo("dead_acute")
    expect(log, "a dead key alone sends nothing", b"")
    combo("q")
    expect(log, "what the table does not have sends nothing", b"")
    combo("a")
    expect(log, "and the next letter is a plain one", b"a")
    log.close()


def test_pending_and_other_keys():
    """A key that goes past the compose state must end what was begun, or a dead
    key pressed a while ago still changes the next letter."""
    log = session(b"")
    for name, key, mods, sent in (("Return", "Return", (), b"\r"), ("an arrow", "Left", (), b"\033[D"),
                                  ("Ctrl+C", "c", ("ctrl",), b"\x03"), ("Tab", "Tab", (), b"\t"),
                                  ("Escape", "Escape", (), b"\033"), ("BackSpace", "BackSpace", (), b"\x7f"),
                                  ("F5", "F5", (), b"\033[15~")):
        combo("dead_acute")
        combo(key, *mods)
        combo("a")
        expect(log, f"{name} ends a dead key", sent + b"a")
    log.close()


def test_pending_and_focus():
    log = session(b"")
    combo("dead_acute")
    with Window("sleep", "100", app_id="other", geometry="10x2"):
        time.sleep(0.2)
    time.sleep(0.3)
    combo("a")
    expect(log, "losing the focus ends a dead key", b"a")
    log.close()


def hold(key, ms, *mods):
    a = []
    for m in mods:
        a += ["-M", m]
    a += ["-P", key, "-s", str(ms), "-p", key]
    for m in reversed(mods):
        a += ["-m", m]
    return subprocess.Popen(["wtype", "-k", "Shift_L", *a])


def test_repeat():
    log = session(b"")
    # sway's defaults: 600 ms until it starts, then 25 a second
    hold("x", 300).wait()
    time.sleep(0.3)
    expect(log, "held shorter than the delay: one x", b"x")
    hold("x", 1100).wait()
    got = log.take()
    time.sleep(0.5)
    later = log.take(b"")
    check(set(got) == {ord("x")} and 5 <= len(got) <= 20, "held for 1.1 s it repeats", repr(got))
    check(later == b"", "and stops with the release", repr(later))
    hold("Left", 1000).wait()
    got = log.take()
    check(len(got) >= 6 and got == b"\033[D" * (len(got) // 3), "an arrow repeats its whole sequence", repr(got))
    hold("x", 900, "ctrl").wait()
    got = log.take()
    check(len(got) >= 3 and set(got) == {0x18}, "Ctrl+X repeats too", repr(got))
    hold("Shift_R", 1000).wait()
    time.sleep(0.2)
    expect(log, "a modifier alone does not", b"")
    log.close()


def test_repeat_and_focus():
    log = session(b"")
    w = hold("x", 2800)
    time.sleep(1.2)
    other = Window("sleep", "100", app_id="other", geometry="10x2")
    time.sleep(0.2)
    log.take()
    time.sleep(0.8)
    got = log.take(b"")
    check(got == b"", "losing the focus ends the repeat", f"{len(got)} more bytes")
    w.wait()
    other.close()
    time.sleep(0.3)
    log.take(b"")
    log.close()


def test_repeat_keyboard_gone():
    """A keyboard that goes away with a key down, as a pulled USB plug does: nobody
    will ever send the release. Another keyboard stays on the seat here."""
    log = session(b"")
    w = hold("x", 8000)
    time.sleep(1.2)
    w.send_signal(signal.SIGKILL)
    w.wait()
    time.sleep(0.3)
    log.take()
    time.sleep(1.0)
    got = log.take(b"")
    check(got == b"", "a keyboard gone with a key down ends the repeat", f"{len(got)} more bytes in 1 s")
    log.close()


def main():
    path = os.path.join(mwtlib.DIR, "Compose")
    with open(path, "w", encoding="utf-8") as f:
        f.write(COMPOSE)
    os.environ["XCOMPOSEFILE"] = path
    # Its virtual keyboard keeps one on the seat, without one no window has focus.
    mouse = Mouse()
    test_compose()
    test_pending_and_other_keys()
    test_pending_and_focus()
    test_repeat()
    test_repeat_and_focus()
    test_repeat_keyboard_gone()
    mouse.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
