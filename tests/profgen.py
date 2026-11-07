#!/usr/bin/env python3
"""The floods for `make profile`, 100 MB each: writes one file per load to the
directory it gets. The seed is fixed, so runs compare."""
import glob
import os
import random
import sys

DIR = sys.argv[1]
SIZE = 100_000_000
CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ,.;:-_()"
random.seed(1)


def text(n):
    return "".join(random.choice(CHARS) for _ in range(n))


def repeat(name, chunk):
    with open(os.path.join(DIR, name), "wb") as f:
        n = 0
        while n < SIZE:
            f.write(chunk)
            n += len(chunk)


def colored():
    lines = ("".join(f"\033[38;5;{random.randrange(256)}m" + text(5) for _ in range(30)) + "\033[0m\n"
             for _ in range(50))
    return "".join(lines)


def screen(k):
    return "".join(f"\033[{y + 1};1H\033[3{(y + k) % 8}m" + text(200) for y in range(55))


repeat("yes", b"y\n" * 4096)
with open(os.path.join(DIR, "seq"), "w") as f:
    i = n = 0
    while n < SIZE:
        i += 1
        n += f.write(f"{i}\n")
repeat("text", "".join(text(190) + "\n" for _ in range(2000)).encode())
repeat("sgr", "".join(colored() for _ in range(40)).encode())
repeat("cjk", "".join("".join(chr(random.randrange(0x4e00, 0x5000)) for _ in range(95)) + "\n"
                      for _ in range(500)).encode())
repeat("screen", ("\033[?1049h" + "".join(screen(k) for k in range(100))).encode())
with open(os.path.join(DIR, "cup"), "w") as f:
    n = 0
    while n < SIZE:
        n += f.write(f"\033[{random.randrange(1, 56)};{random.randrange(1, 201)}Hx")
videos = sorted(glob.glob(os.path.join(sys.argv[2], "**/*.vt"), recursive=True))
if videos:
    repeat("videos", b"".join(open(v, "rb").read() for v in videos))
