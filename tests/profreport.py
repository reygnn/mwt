#!/usr/bin/env python3
"""Reads what tests/profsampler.c wrote, for `make profile`: per function how much
of mwt's CPU time was spent in it (self) and under it (with callees), and the
source lines that cost most. A sample that ends in a library counts for that
library in self, and for the line in mwt that called it in the lines.
usage: profreport.py prefix mwt-binary"""
import bisect
import collections
import struct
import subprocess
import sys

prefix, binary = sys.argv[1:3]
FUNCS = ["read_pty", "vt_write", "scroll_up", "push_hist", "draw", "draw_row", "blit", "font_glyph"]

maps = []
with open(prefix + ".maps") as f:
    for line in f:
        p = line.split()
        if len(p) >= 6 and "x" in p[1]:
            lo, hi = (int(x, 16) for x in p[0].split("-"))
            maps.append((lo, hi, int(p[2], 16), p[5]))
out = subprocess.run(["nm", "-n", "--defined-only", binary], capture_output=True, text=True).stdout
symbols = sorted((int(m[0], 16), m[2]) for m in map(str.split, out.splitlines())
                 if len(m) >= 3 and m[1] in "tTwW")
addrs = [s[0] for s in symbols]
base = next(lo - off for lo, hi, off, path in maps if path == binary)
names = {}


def name(addr):
    if addr not in names:
        path = next((p for lo, hi, _, p in maps if lo <= addr < hi), None)
        if path == binary:
            i = bisect.bisect_right(addrs, addr - base) - 1
            names[addr] = symbols[i][1] if i >= 0 else "?"
        else:
            names[addr] = "[" + (path or "kernel").rsplit("/", 1)[-1].split(".so")[0] + "]"
    return names[addr]


with open(prefix + ".raw", "rb") as f:
    data = f.read()
words = struct.unpack(f"<{len(data) // 8}Q", data)
self_ = collections.Counter()
under = collections.Counter()
lines = collections.Counter()
total = 0
i = 0
while i < len(words):
    n = words[i]
    stack = words[i + 1:i + 1 + n]
    i += 1 + n
    total += 1
    who = [name(a) for a in stack]
    self_[who[0]] += 1
    for w in set(who):
        under[w] += 1
    for j, a in enumerate(stack):
        if not who[j].startswith("["):
            lines[a - base - (1 if j else 0)] += 1  # a return address is past its call
            break
total = max(total, 1)


def pct(v):
    return f"{100 * v / total:.1f}"


print("  self:  " + "  ".join(f"{k} {pct(v)}" for k, v in self_.most_common(8)))
print("  under: " + "  ".join(f"{k} {pct(under[k])}" for k in FUNCS if under[k]))
hot = lines.most_common(4000)
out = subprocess.run(["addr2line", "-e", binary] + [hex(a) for a, _ in hot],
                     capture_output=True, text=True).stdout.splitlines()
by_line = collections.Counter()
for (_, v), where in zip(hot, out):
    by_line[where.rsplit("/", 1)[-1].split(" ")[0]] += v
print("  lines: " + "  ".join(f"{k} {pct(v)}" for k, v in by_line.most_common(8)))
