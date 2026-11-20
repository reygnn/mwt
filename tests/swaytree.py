#!/usr/bin/env python3
"""A window in sway's tree, found by its app_id: the one walk of the tree that
tests/lib.sh and tests/mwtlib.py share.

    swaymsg -t get_tree | tests/swaytree.py rect|size|title|urgent APP_ID

rect prints "x y width height", size "width height", title the name and urgent
"true" or "false"; nothing when there is no such window or no tree."""
import json
import subprocess
import sys


def walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from walk(c)


def find(tree, app_id):
    for n in walk(tree):
        if n.get("app_id") == app_id:
            return n
    return None


def node(app_id="mwt"):
    """The window's node in sway's tree now, or None."""
    out = subprocess.run(["swaymsg", "-t", "get_tree"], capture_output=True, text=True).stdout
    try:
        return find(json.loads(out), app_id)
    except ValueError:
        return None


def main():
    what, app_id = sys.argv[1], sys.argv[2]
    try:
        n = find(json.load(sys.stdin), app_id)
    except ValueError:
        return 0
    if n is None:
        return 0
    r = n["rect"]
    if what == "rect":
        print(r["x"], r["y"], r["width"], r["height"])
    elif what == "size":
        print(r["width"], r["height"])
    elif what == "title":
        print(n.get("name") or "")
    elif what == "urgent":
        print("true" if n.get("urgent") else "false")
    return 0


if __name__ == "__main__":
    sys.exit(main())
