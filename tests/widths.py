#!/usr/bin/env python3
"""Sample strings whose width terminals and programs disagree about, and the
tools tests/width.sh uses to measure and check them.

  widths.py list FILE         one sample per line, for vim and tmux to show
  widths.py prompt            the samples run together, for PS1
  widths.py probe OUT         inside mwt: the column after each sample, via CPR
  widths.py vim OUT           vim's strdisplaywidth() of each sample
  widths.py tmux OUT          tmux's cursor_x after printing each sample
  widths.py table MWT VIM TMUX    the widths side by side, OK/GAP/INFO lines
  widths.py tmux-list FILE W TMUX  the samples filled to W columns, as tmux counts
  widths.py tmux-check SHOWN W TMUX  rows of a vsplit: dots up to the border at W
  widths.py vim-check BEFORE AFTER  rows before and after `:%normal $rX`
  widths.py prompt-visible    the prompt as mwt shows it

What mwt counts is glibc's wcwidth(), with zero width characters dropped.
bash's readline counts the same way, so bash and mwt agree by construction.
"""
import ctypes
import json
import os
import subprocess
import sys
import termios
import time
import tty

SAMPLES = [
    ("ascii", "abc"),
    ("umlaut", "äöü"),
    ("e+acute", "é"),           # é decomposed, as macOS file names are
    ("cjk", "漢字"),
    ("smiley", "\U0001F642"),
    ("heart+vs16", "❤️"),   # text default, VS16 asks for emoji
    ("warn+vs16", "⚠️"),
    ("thumb+skin", "\U0001F44D\U0001F3FD"),
    ("family zwj", "\U0001F468‍\U0001F469‍\U0001F467"),
    ("flag de", "\U0001F1E9\U0001F1EA"),
    ("hangul jamo", "한"),  # 한 as three jamo
    ("zwsp", "a​b"),
    ("thai", "ที่"),
    ("u16 emoji", "\U0001FAE9"),      # Unicode 16, newer than many tables
]
MARK = "|end"

libc = ctypes.CDLL("libc.so.6")
libc.wcwidth.argtypes = [ctypes.c_wchar]


def mwt_width(s):
    """What vt.c does: below U+0300 one cell, else wcwidth(), -1 counts 1."""
    w = 0
    for c in s:
        cp = ord(c)
        if cp < 0x300:
            w += 1
        else:
            x = libc.wcwidth(c)
            w += 1 if x < 0 else x
    return w


def visible(s):
    """s as mwt shows and copies it: zero width characters are dropped."""
    return "".join(c for c in s if ord(c) < 0x300 or libc.wcwidth(c) != 0)


def probe(out):
    fd = os.open("/dev/tty", os.O_RDWR)
    old = termios.tcgetattr(fd)
    tty.setraw(fd)
    res = {}
    try:
        for name, s in SAMPLES:
            os.write(fd, ("\r\033[2K" + s + "\033[6n").encode())
            reply = b""
            while not reply.endswith(b"R"):
                reply += os.read(fd, 1)
            row, col = reply[reply.rindex(b"[") + 1:-1].split(b";")
            res[name] = int(col) - 1
    finally:
        termios.tcsetattr(fd, termios.TCSANOW, old)
    os.write(fd, b"\r\n")
    json.dump(res, open(out, "w"))


def vim_widths(out):
    res = {}
    tmp = out + ".txt"
    for name, s in SAMPLES:
        open(tmp, "w").write(s + "\n")
        # vim writes what it cannot print as <200d>, so what it sends is
        # strtrans() of the text, and that is what mwt has to agree with
        r = subprocess.run(["vim", "-es", "-u", "NONE", "-N", "-c",
                            'call writefile([strdisplaywidth(getline(1)), '
                            'strtrans(getline(1))], "/dev/stdout")',
                            "-c", "qa!", tmp], capture_output=True, text=True)
        w, shown = r.stdout.rstrip("\n").split("\n", 1)
        res[name] = {"width": int(w), "sends": shown}
    json.dump(res, open(out, "w"))


def tmux_widths(out):
    res = {}
    tmp = out + ".txt"
    for i, (name, s) in enumerate(SAMPLES):
        sock = "widths%d-%d" % (os.getpid(), i)  # a fresh server each time
        open(tmp, "w").write(s)
        subprocess.run(["tmux", "-L", sock, "-f", "/dev/null", "new-session", "-d",
                        "-x", "100", "-y", "5", "cat '%s'; exec sleep 30" % tmp])
        x = ""
        for _ in range(50):
            time.sleep(0.05)
            x = subprocess.run(["tmux", "-L", sock, "display", "-p", "#{cursor_x}"],
                               capture_output=True, text=True).stdout.strip()
            if x not in ("", "0"):
                break
        subprocess.run(["tmux", "-L", sock, "kill-server"], capture_output=True)
        res[name] = int(x or 0)
    json.dump(res, open(out, "w"))


def table(mwt_f, vim_f, tmux_f):
    mwt = json.load(open(mwt_f))
    vim = json.load(open(vim_f))
    tmx = json.load(open(tmux_f))
    # vim: its width against what mwt makes of the text vim sends
    print("INFO  %-12s %4s %4s %9s %4s" % ("sample", "mwt", "bash", "vim", "tmux"))
    for name, s in SAMPLES:
        m = mwt.get(name, -1)
        v = vim[name]
        vs = "%d" % v["width"] if v["sends"] == s else "%d<%d>" % (v["width"], mwt_width(v["sends"]))
        print("INFO  %-12s %4d %4d %9s %4d" % (name, m, mwt_width(s), vs, tmx[name]))
    for name, s in SAMPLES:
        m = mwt.get(name, -1)
        if m != mwt_width(s):
            print("GAP   %s: mwt puts the cursor at %d, its own model says %d" % (name, m, mwt_width(s)))
        off = []
        v = vim[name]
        if v["width"] != mwt_width(v["sends"]):
            off.append("vim counts %d where mwt draws %d" % (v["width"], mwt_width(v["sends"])))
        if tmx[name] != m:
            off.append("tmux counts %d where mwt draws %d" % (tmx[name], m))
        if off:
            print("GAP   %s: %s" % (name, ", ".join(off)))
        else:
            print("OK    %s: %d everywhere" % (name, m))


def tmux_list(out, width, tmux_f):
    """Each sample row filled with dots up to the pane edge, as tmux counts."""
    tmx = json.load(open(tmux_f))
    with open(out, "w") as f:
        for name, s in SAMPLES:
            f.write(s + MARK + "." * max(0, width - tmx[name] - len(MARK)) + "\n")


def tmux_check(shown_f, width, tmux_f):
    """tmux writes a row cell by cell: if mwt draws a sample wider or narrower
    than tmux counts, the dots run into the border or stop short of it."""
    tmx = json.load(open(tmux_f))
    rows = open(shown_f, encoding="utf-8", errors="replace").read().split("\n")
    for (name, s), row in zip(SAMPLES, rows):
        want = visible(s) + MARK + "." * max(0, width - tmx[name] - len(MARK))
        if "│" not in row:
            print("GAP   tmux %s: the border is gone from the row: %r" % (name, row))
            continue
        left = row[:row.index("│")]
        col = mwt_width(left)
        if col == width and left == want:
            print("OK    tmux %s: row and border in place" % name)
        elif col != width:
            print("GAP   tmux %s: border at column %d instead of %d: %r" % (name, col, width, row))
        else:
            print("GAP   tmux %s: border in place, the row is off: %r" % (name, left))


def vim_check(before_f, after_f):
    """After `:%normal $rX` each sample row must be itself with the d an X."""
    read = lambda f: open(f, encoding="utf-8", errors="replace").read().split("\n")
    before, after = read(before_f), read(after_f)
    for i, (name, s) in enumerate(SAMPLES):
        b = before[i] if i < len(before) else ""
        a = after[i] if i < len(after) else ""
        want = b[:-1] + "X" if b.endswith("d") else None
        if want is None:
            print("GAP   vim %s: the row before was already off: %r" % (name, b))
        elif a == want:
            print("OK    vim %s: the X lands on the d" % name)
        else:
            print("GAP   vim %s: %r instead of %r" % (name, a, want))


def main():
    cmd = sys.argv[1]
    if cmd == "list":
        open(sys.argv[2], "w").write("".join(s + MARK + "\n" for _, s in SAMPLES))
    elif cmd == "prompt":
        sys.stdout.write("".join(s for _, s in SAMPLES))
    elif cmd == "prompt-visible":
        sys.stdout.write(visible("".join(s for _, s in SAMPLES)))
    elif cmd == "names":
        print("\n".join(n for n, _ in SAMPLES))
    elif cmd == "probe":
        probe(sys.argv[2])
    elif cmd == "vim":
        vim_widths(sys.argv[2])
    elif cmd == "tmux":
        tmux_widths(sys.argv[2])
    elif cmd == "table":
        table(*sys.argv[2:5])
    elif cmd == "vim-check":
        vim_check(sys.argv[2], sys.argv[3])
    elif cmd == "tmux-list":
        tmux_list(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    elif cmd == "tmux-check":
        tmux_check(sys.argv[2], int(sys.argv[3]), sys.argv[4])
    else:
        sys.exit("unknown command " + cmd)


if __name__ == "__main__":
    main()
