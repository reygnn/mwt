#!/usr/bin/env python3
"""What mwt sends for keys and the mouse, against what mwt.info promises for them
and what programs expect otherwise. A program in raw mode logs each read in hex.
Run with `make keys`, which goes through tests/headless.sh. Every k* capability of
the entry must be listed here."""
import os
import sys
import tempfile
import time

import mwtlib
from mwtlib import Mouse, Window, check, combo, fail, finish, rect
import terminfo

LOGGER = r"""
import os, sys, termios, tty
log, init = sys.argv[1], bytes.fromhex(sys.argv[2])
tty.setraw(0)
os.write(1, init)
with open(log, "a") as f:
    f.write("ready\n")
    f.flush()
    while True:
        b = os.read(0, 4096)
        if not b:
            break
        f.write(b.hex() + "\n")
        f.flush()
"""

# capability: key, modifiers
CAP_KEYS = {
    "kbs": ("BackSpace",), "kcbt": ("Tab", "shift"), "kcub1": ("Left",), "kcud1": ("Down",),
    "kcuf1": ("Right",), "kcuu1": ("Up",), "kdch1": ("Delete",), "kend": ("End",),
    "khome": ("Home",), "kich1": ("Insert",), "knp": ("Next",), "kpp": ("Prior",),
    "kDC": ("Delete", "shift"), "kEND": ("End", "shift"), "kHOM": ("Home", "shift"),
    "kLFT": ("Left", "shift"), "kRIT": ("Right", "shift"), "kind": ("Down", "shift"),
    "kri": ("Up", "shift"),
}
for n in range(1, 13):
    CAP_KEYS[f"kf{n}"] = (f"F{n}",)
    CAP_KEYS[f"kf{n + 12}"] = (f"F{n}", "shift")

# Declared, but mwt keeps these keys for itself (paste, scrollback), on every screen.
TAKEN_BY_MWT = {"kIC": ("Insert", "shift"), "kNXT": ("Next", "shift"), "kPRV": ("Prior", "shift")}
# Checked below by events, not by a key.
BY_EVENT = {"kmous", "kxIN", "kxOUT"}

# What programs without terminfo expect, in normal mode: key and modifiers, bytes
PLAIN = [
    (("Up",), b"\033[A"), (("Down",), b"\033[B"), (("Right",), b"\033[C"), (("Left",), b"\033[D"),
    (("Home",), b"\033[H"), (("End",), b"\033[F"),
    (("Up", "ctrl"), b"\033[1;5A"), (("Left", "alt"), b"\033[1;3D"),
    (("Right", "shift", "alt"), b"\033[1;4C"), (("Down", "ctrl", "alt"), b"\033[1;7B"),
    (("Delete", "ctrl"), b"\033[3;5~"), (("Prior", "ctrl"), b"\033[5;5~"), (("F5", "alt"), b"\033[15;3~"),
    (("F1", "ctrl"), b"\033[1;5P"), (("F4", "shift", "alt"), b"\033[1;4S"),
    (("Return",), b"\r"), (("Return", "alt"), b"\033\r"), (("KP_Enter",), b"\r"),
    (("Tab",), b"\t"), (("Escape",), b"\033"), (("BackSpace",), b"\x7f"),
    (("BackSpace", "ctrl"), b"\x08"), (("BackSpace", "alt"), b"\033\x7f"),
    (("a",), b"a"), (("a", "ctrl"), b"\x01"), (("z", "ctrl"), b"\x1a"), (("x", "alt"), b"\033x"),
    (("c", "ctrl", "alt"), b"\033\x03"), (("space", "ctrl"), b"\x00"), (("adiaeresis",), "ä".encode()),
    (("KP_Up",), b"\033[A"), (("KP_Home",), b"\033[H"), (("KP_Delete",), b"\033[3~"),
    (("KP_Prior",), b"\033[5~"),
]


class Logger:
    def __init__(self, init=b"", **kw):
        global _n
        _n = globals().get("_n", 0) + 1
        self.log = os.path.join(mwtlib.DIR, f"keys{_n}.log")
        self.script = os.path.join(mwtlib.DIR, "keylogger.py")
        with open(self.script, "w") as f:
            f.write(LOGGER)
        self.win = Window("python3", self.script, self.log, init.hex(), **kw)
        for _ in range(100):
            if os.path.exists(self.log) and os.path.getsize(self.log):
                break
            time.sleep(0.05)
        self.seen = self._read()

    def _read(self):
        with open(self.log) as f:
            return f.read().split()[1:]

    def take(self, want=None):
        """What arrived since the last call, waiting a little for what is due."""
        for _ in range(20):
            lines = self._read()
            got = bytes.fromhex("".join(lines[len(self.seen):]))
            if want is not None and got == want:
                break
            time.sleep(0.03)
        self.seen = lines
        return got

    def close(self):
        self.win.close()


def press(log, what, spec, want):
    combo(spec[0], *spec[1:])
    got = log.take(want)
    check(got == want, what, f"got {got!r}, expected {want!r}")


def main():
    # Its virtual keyboard keeps one on the seat. Without one no window has focus.
    mouse = Mouse()
    with tempfile.TemporaryDirectory() as d:
        caps = terminfo.compile_entry(d)
    # the entry for mwt itself stays where headless.sh put it
    os.environ.pop("TERMINFO", None)
    known = set(CAP_KEYS) | set(TAKEN_BY_MWT) | BY_EVENT
    for name in sorted(n for n in caps if n.startswith("k")):
        if name not in known:
            fail(f"{name}: in mwt.info, but no key here")
    for name in sorted(known - set(caps)):
        fail(f"{name}: listed here, but not in mwt.info")

    # As programs using terminfo run: smkx, on the alternate screen where mwt leaves
    # Shift+Home and Shift+End to the program.
    log = Logger(caps["smkx"] + caps["smcup"])
    for name, spec in sorted(CAP_KEYS.items()):
        press(log, f"{name}: {'+'.join(reversed(spec))}", spec, caps[name])
    for name, spec in sorted(TAKEN_BY_MWT.items()):
        combo(spec[0], *spec[1:])
        got = log.take()
        if got == caps[name]:
            fail(f"{name}: mwt no longer keeps this key, take it off TAKEN_BY_MWT")
        else:
            print(f"info: {name}: {'+'.join(reversed(spec))} never reaches a program, mwt keeps it")
    log.close()

    log = Logger()
    for spec, want in PLAIN:
        press(log, f"normal mode: {'+'.join(reversed(spec))}", spec, want)
    log.close()

    # Focus: kxOUT when another window takes it, kxIN when it comes back.
    log = Logger(b"\033[?1004h")
    log.take()
    other = Window("sleep", "100", app_id="other", geometry="10x2")
    time.sleep(0.2)
    got = log.take(caps["kxOUT"])
    check(got == caps["kxOUT"], "kxOUT: focus goes to another window", repr(got))
    other.close()
    time.sleep(0.2)
    got = log.take(caps["kxIN"])
    check(got == caps["kxIN"], "kxIN: focus comes back", repr(got))
    log.close()

    # kmous and the format xm gives: press and release in SGR form, 1-based. xm
    # numbers its first two parameters by what the reports hold, column and row.
    log = Logger(terminfo.curses.tparm(caps["XM"], 1))
    log.take()
    x, y, w, h = rect()
    cw, ch = (w - 2 * mwtlib.PAD) // 80, (h - 2 * mwtlib.PAD) // 24
    col, row = 7, 3
    mouse.send("move", x + mwtlib.PAD + col * cw + cw // 2, y + mwtlib.PAD + row * ch + ch // 2)
    time.sleep(0.2)
    mouse.send("click", "left")
    time.sleep(0.2)
    down = terminfo.curses.tparm(caps["xm"], col, row, 0, 1)
    up = terminfo.curses.tparm(caps["xm"], col, row, 0, 0)
    got = log.take(down + up)
    check(got.startswith(caps["kmous"]), "kmous: a report starts with it", repr(got))
    check(got == down + up, "xm: press and release as declared", f"got {got!r}, expected {down + up!r}")
    mouse.close()
    log.close()
    return finish()


if __name__ == "__main__":
    sys.exit(main())
