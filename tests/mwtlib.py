"""What tests/pixel.py and tests/keys.py share, under tests/headless.sh: windows,
keys, the mouse, screenshots and the reporting of results."""
import json
import os
import re
import subprocess
import time

DIR = os.environ["DIR"]
MWT = os.environ["MWT"]
LOG = os.path.join(DIR, "log")
PAD = 2  # config.h padding, logical pixels
fails = 0
_count = 0


def ok(what):
    print(f"ok: {what}")


def fail(what):
    global fails
    fails += 1
    print(f"FAIL: {what}")


def check(cond, what, detail=""):
    if cond:
        ok(what)
    else:
        fail(f"{what}{': ' + detail if detail else ''}")
    return cond


def finish():
    if os.path.exists(LOG):
        with open(LOG, errors="replace") as f:
            log = f.read()
        m = re.search(r"AddressSanitizer|runtime error", log)
        if m:
            fail("sanitizer report from mwt:\n" + log[m.start():m.start() + 1500])
    print("PASS" if not fails else f"=== {fails} checks failed ===")
    return 1 if fails else 0


def _walk(n):
    yield n
    for k in ("nodes", "floating_nodes"):
        for c in n.get(k, []):
            yield from _walk(c)


def swaymsg(*args):
    subprocess.run(["swaymsg", "-q", *args], check=False)


def rect(app_id="mwt"):
    """x, y, width, height in logical pixels, or None."""
    tree = json.loads(subprocess.run(["swaymsg", "-t", "get_tree"], capture_output=True, text=True).stdout)
    for n in _walk(tree):
        if n.get("app_id") == app_id:
            r = n["rect"]
            return r["x"], r["y"], r["width"], r["height"]
    return None


class Image:
    def __init__(self, ppm, scale=1):
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+255\s", ppm)
        self.w, self.h = int(m.group(1)), int(m.group(2))
        self.data = ppm[m.end():]
        self.scale = scale

    def px(self, x, y):
        i = (y * self.w + x) * 3
        return self.data[i] << 16 | self.data[i + 1] << 8 | self.data[i + 2]

    def __eq__(self, other):
        return (self.w, self.h, self.data) == (other.w, other.h, other.data)


def shot(app_id="mwt", scale=1):
    x, y, w, h = rect(app_id)
    png = subprocess.run(["grim", "-t", "ppm", "-g", f"{x},{y} {w}x{h}", "-"],
                         capture_output=True, check=True).stdout
    return Image(png, scale)


def settle(app_id="mwt", scale=1, quiet=0.25, limit=6.0):
    """A screenshot once the window has stopped changing."""
    end = time.time() + limit
    last = shot(app_id, scale)
    since = time.time()
    while time.time() < end:
        time.sleep(0.05)
        cur = shot(app_id, scale)
        if cur != last:
            last, since = cur, time.time()
        elif time.time() - since >= quiet:
            break
    return last


class Window:
    """mwt with a program in raw mode that writes what feed() gets, unchanged."""

    def __init__(self, *args, app_id="mwt", geometry="80x24"):
        global _count
        _count += 1
        self.app_id = app_id
        self.fifo = os.path.join(DIR, f"fifo{_count}")
        os.mkfifo(self.fifo)
        log = open(LOG, "a")
        prog = args or ("sh", "-c", f"stty raw -echo; exec cat '{self.fifo}'")
        self.proc = subprocess.Popen([MWT, "-a", app_id, "-g", geometry, "-e", *prog],
                                     stdout=log, stderr=log, cwd=os.environ["HOME"])
        log.close()
        self.out = None
        if not args:
            self.out = open(self.fifo, "wb", buffering=0)
        for _ in range(100):
            if rect(app_id):
                break
            time.sleep(0.05)
        time.sleep(0.3)

    def feed(self, data):
        self.out.write(data)

    def close(self):
        if self.out:
            self.out.close()
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
        for _ in range(100):
            if not rect(self.app_id):
                break
            time.sleep(0.05)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def keys(*args):
    """A wtype call. Its first key is one mwt ignores: sway hands a new keyboard's
    first key over in wl_keyboard.enter, which a client must not take as a press."""
    subprocess.run(["wtype", "-k", "Shift_L", *args], check=True)
    time.sleep(0.15)


def combo(key, *mods):
    a = []
    for m in mods:
        a += ["-M", m]
    a += ["-k", key]
    for m in reversed(mods):
        a += ["-m", m]
    keys(*a)


class Mouse:
    def __init__(self):
        self.p = subprocess.Popen([os.environ["VINPUT"], "1000", "700"], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, text=True, bufsize=1)

    def send(self, *cmd):
        self.p.stdin.write(" ".join(map(str, cmd)) + "\n")
        self.p.stdin.flush()
        self.p.stdout.readline()

    def close(self):
        self.p.stdin.close()
        self.p.terminate()
        self.p.wait()
