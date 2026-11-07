#!/usr/bin/env python3
"""One update per 60 Hz tick for 8 seconds, as a full-screen program would write
them, for `make profile`: what a frame costs when nothing floods the terminal.
usage: profpaced.py cell|scroll|full file; the file gets how many it managed."""
import os
import random
import sys
import time

mode, report = sys.argv[1:3]
size = os.get_terminal_size(1)
rows, cols = size.lines, size.columns
CHARS = "abcdefghijklmnopqrstuvwxyz ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
random.seed(2)
pool = ["".join(random.choice(CHARS) for _ in range(cols)) for _ in range(400)]
out = os.fdopen(1, "w", buffering=1 << 20, newline="")
if mode != "scroll":
    out.write("\033[?1049h\033[2J")
start = time.monotonic()
k = 0
while time.monotonic() - start < 8:
    if mode == "full":
        o = "".join(f"\033[{y + 1};1H" + pool[(k + y * 7) % 400] for y in range(rows))
    elif mode == "cell":
        o = f"\033[{k % rows + 1};{k % cols + 1}H" + CHARS[k % len(CHARS)]
    else:
        o = pool[k % 400][:120] + "\r\n"
    out.write(o)
    out.flush()
    k += 1
    pause = start + k / 60 - time.monotonic()
    if pause > 0:
        time.sleep(pause)
with open(report, "w") as f:
    f.write(f"{k}\n")
