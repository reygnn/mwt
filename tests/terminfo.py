#!/usr/bin/env python3
"""mwt.info says it declares only what vt.c implements. This checks that: each
capability is expanded by ncurses and fed to the VT core (vtdump), and what it
did is compared with what it means. A capability without a check fails too, so a
new one cannot slip in untested. What mwt sends (keys, mouse, paste) is not the
VT core's: tests/keys.py checks those and INPUT lists them."""
import curses
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VTDUMP = os.environ.get("VTDUMP", os.path.join(ROOT, "vtdump"))
COLS, ROWS = 20, 6


def parse_entry(terminfo_dir):
    """The entry as {name: True | int | bytes}, unexpanded."""
    env = dict(os.environ, TERMINFO=terminfo_dir)
    out = subprocess.run(["infocmp", "-1", "-x", "mwt"], env=env, check=True,
                         capture_output=True, text=True).stdout
    caps = {}
    for line in out.splitlines():
        if not line.startswith("\t"):
            continue
        item = line.strip()[:-1]  # the comma
        if "=" in item:
            name, value = item.split("=", 1)
            caps[name] = unescape(value)
        elif "#" in item:
            name, value = item.split("#", 1)
            caps[name] = int(value, 0)
        else:
            caps[item] = True
    return caps


def unescape(s):
    named = {"E": 27, "e": 27, "n": 10, "r": 13, "t": 9, "b": 8, "f": 12, "s": 32, "l": 10}
    out = bytearray()
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\":
            i += 1
            c = s[i]
            if c in named:
                out.append(named[c])
            elif c.isdigit():
                out.append(int(s[i:i + 3], 8))
                i += 2
            else:
                out += c.encode()
        elif c == "^":
            i += 1
            out.append(ord(s[i]) & 0x1f if s[i] != "?" else 0x7f)
        else:
            out += c.encode()
        i += 1
    return bytes(out)


def compile_entry(directory):
    subprocess.run(["tic", "-x", "-o", directory, os.path.join(ROOT, "mwt.info")], check=True)
    os.environ["TERMINFO"] = directory
    os.environ.pop("TERMINFO_DIRS", None)
    with open(os.devnull, "w") as null:
        curses.setupterm("mwt", null.fileno())
    return parse_entry(directory)


class Screen:
    def __init__(self, data, cols=COLS, rows=ROWS):
        p = subprocess.run([VTDUMP, str(cols), str(rows)], input=data, capture_output=True, check=True)
        self.__dict__.update(json.loads(p.stdout))
        self.reply = bytes.fromhex(self.reply)

    def row(self, r):
        wdummy = self.attrbits["wdummy"]
        return "".join(chr(c[0]) for c in self.cells[r] if not c[3] & wdummy).rstrip(" ")

    def rows(self):
        return [self.row(r) for r in range(len(self.cells))]

    def attr(self, r, x, name):
        return bool(self.cells[r][x][3] & self.attrbits[name])

    def fg(self, r, x):
        return self.cells[r][x][1]

    def bg(self, r, x):
        return self.cells[r][x][2]


class Failed(Exception):
    pass


def eq(got, want):
    if got != want:
        raise Failed(f"got {got!r}, expected {want!r}")


def yes(cond, what):
    if not cond:
        raise Failed(what)


CAPS = {}


def S(cap, *params):
    """A string capability, expanded."""
    return curses.tparm(CAPS[cap], *params)


def at(row, col):
    return b"\033[%d;%dH" % (row + 1, col + 1)


FOUR = b"aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc\r\ndddddddddd"


def sgr_on_off(on, off, name):
    def check():
        s = Screen(S(on) + b"x" + S(off) + b"y")
        yes(s.attr(0, 0, name), f"{on} does not set {name}")
        yes(not s.attr(0, 1, name), f"{off} does not clear {name}")
    return check


def sgr_on(on, name):
    def check():
        s = Screen(S(on) + b"x")
        yes(s.attr(0, 0, name), f"{on} does not set {name}")
    return check


def mode_set(cap, *modes, off=False):
    def check():
        got = Screen(("\033[?2004h\033[?1h\033=\033[?1000;1006h\033[?25l\033[?1049h".encode() if off else b"") + S(cap)).modes
        for m in modes:
            yes((m in got) != off, f"{m} {'still' if off else 'not'} set: {got}")
    return check


def moved(cap, params, start, want):
    def check():
        s = Screen(at(*start) + S(cap, *params))
        eq((s.cy, s.cx), want)
    return check


def check_acsc():
    # VT100 name to Unicode for what the entry maps
    names = {"`": 0x25c6, "a": 0x2592, "f": 0xb0, "g": 0xb1, "j": 0x2518, "k": 0x2510,
             "l": 0x250c, "m": 0x2514, "n": 0x253c, "q": 0x2500, "t": 0x251c, "u": 0x2524,
             "v": 0x2534, "w": 0x252c, "x": 0x2502, "~": 0xb7}
    pairs = CAPS["acsc"].decode()
    for i in range(0, len(pairs), 2):
        vt100, sent = pairs[i], pairs[i + 1]
        s = Screen(S("smacs") + sent.encode())
        cp = s.cells[0][0][0]
        yes(cp > 127, f"acsc {vt100!r} stays ASCII")
        if vt100 in names:
            eq(cp, names[vt100])


def check_sgr():
    # p1 standout, p2 underline, p3 reverse, p4 blink, p5 dim, p6 bold, p7 invis
    table = {0: "reverse", 1: "underline", 2: "reverse", 3: "blink", 4: "dim", 5: "bold", 6: "invisible"}
    all_attrs = set(table.values())
    for i, name in table.items():
        params = [0] * 9
        params[i] = 1
        s = Screen(S("smul") + S("bold") + S("sgr", *params) + b"x")
        for other in all_attrs:
            eq((other, s.attr(0, 0, other)), (other, other == name))
    params = [0] * 9
    params[8] = 1
    eq(Screen(S("sgr", *params) + b"q").cells[0][0][0], 0x2500)
    eq(Screen(S("sgr", *params) + S("sgr", *[0] * 9) + b"q").cells[0][0][0], ord("q"))


def check_color(cap, column):
    def check():
        for color, cell in ((1, 1), (9, 9), (100, 100), (255, 255)):
            eq(Screen(S(cap, color) + b"x").cells[0][0][column], cell)
    return check


def check_rgb(cap, column):
    def check():
        s = Screen(S(cap, 1, 2, 3) + b"x")
        eq(s.cells[0][0][column], s.rgbflag | 0x010203)
    return check


def check_clear():
    s = Screen(FOUR + S("clear"))
    eq(s.rows(), [""] * ROWS)
    eq((s.cy, s.cx), (0, 0))


def check_am():
    s = Screen(b"x" * (COLS + 1))
    eq(s.rows()[:2], ["x" * COLS, "x"])


def check_xenl():
    s = Screen(b"x" * COLS)
    eq((s.cy, s.cx, s.wrapnext), (0, COLS - 1, True))
    s = Screen(b"x" * COLS + b"\r\ny")
    eq(s.rows()[:3], ["x" * COLS, "y", ""])


def check_bce():
    s = Screen(S("setab", 4) + S("el"))
    eq(s.bg(0, COLS - 1), 4)
    s = Screen(S("setab", 4) + S("clear"))
    eq(s.bg(ROWS - 1, COLS - 1), 4)


def check_mir():
    s = Screen(b"ab\r" + S("smir") + at(0, 1) + b"X" + at(0, 0) + b"Y" + S("rmir"))
    eq(s.row(0), "YaXb")


def check_msgr():
    s = Screen(S("smso") + at(2, 3) + b"x" + S("rmso"))
    yes(s.attr(2, 3, "reverse"), "standout lost by a cursor move")


def check_npc():
    for name, value in CAPS.items():
        yes(not (isinstance(value, bytes) and b"$<" in value), f"{name} asks for padding")


def check_ht_it():
    eq(Screen(b"\t").cx, CAPS["it"])
    eq(Screen(S("ht")).cx, CAPS["it"])


def check_size(cap, const):
    def check():
        with open(os.path.join(ROOT, "config.h")) as f:
            eq(CAPS[cap], int(re.search(const + r"\s*=\s*(\d+)", f.read()).group(1)))
    return check


def text_op(cap, params, row_in, col, want):
    def check():
        s = Screen(row_in + at(0, col) + S(cap, *params))
        eq(s.row(0), want)
        eq((s.cy, s.cx), (0, col))
    return check


def lines_op(cap, params, row, want):
    def check():
        s = Screen(FOUR + at(row, 0) + S(cap, *params))
        eq(s.rows()[:4], want)
    return check


def check_csr():
    s = Screen(at(4, 4) + S("csr", 1, 3))
    eq((s.top, s.bot, s.cy, s.cx), (1, 3, 0, 0))
    s = Screen(b"top" + S("csr", 1, 3) + at(3, 0) + b"L\r\nx")
    eq(s.rows()[:4], ["top", "", "L", "x"])


def check_ed():
    s = Screen(FOUR + at(1, 5) + S("ed"))
    eq(s.rows()[:3], ["a" * 10, "b" * 5, ""])


def check_el():
    eq(Screen(b"abcdef" + at(0, 2) + S("el")).row(0), "ab")


def check_el1():
    eq(Screen(b"abcdef" + at(0, 2) + S("el1")).row(0), "   def")


def check_cr():
    s = Screen(b"abc" + S("cr") + b"x")
    eq((s.row(0), s.cx), ("xbc", 1))


def check_bel():
    eq(Screen(S("bel")).bells, 1)


def check_ind():
    s = Screen(b"top" + at(ROWS - 1, 2) + S("ind"))
    eq((s.row(0), s.cy, s.cx), ("", ROWS - 1, 2))
    eq(s.hist, 1)


def check_indn():
    s = Screen(FOUR + at(0, 0) + S("indn", 2))
    eq(s.rows()[:3], ["c" * 10, "d" * 10, ""])


def check_ri():
    s = Screen(b"top" + at(0, 2) + S("ri"))
    eq((s.row(0), s.row(1), s.cy, s.cx), ("", "top", 0, 2))


def check_rin():
    s = Screen(FOUR + at(3, 0) + S("rin", 2))
    eq(s.rows()[:4], ["", "", "a" * 10, "b" * 10])


def check_sc_rc():
    s = Screen(S("smul") + at(2, 3) + S("sc") + S("sgr0") + at(0, 0) + S("rc") + b"x")
    eq((s.cy, s.cx), (2, 4))
    yes(s.attr(2, 3, "underline"), "rc does not restore the attributes")


def check_op():
    s = Screen(S("setaf", 1) + S("setab", 2) + S("op") + b"x")
    eq((s.fg(0, 0), s.bg(0, 0)), (s.defaultfg, s.defaultbg))


def check_sgr0():
    s = Screen(S("bold") + S("rev") + S("setaf", 1) + S("smacs") + S("sgr0") + b"q")
    eq(s.cells[0][0][0], ord("q"))
    eq(s.cells[0][0][1], s.defaultfg)
    yes(not s.attr(0, 0, "bold") and not s.attr(0, 0, "reverse"), "sgr0 keeps attributes")


def check_smam():
    s = Screen(S("rmam") + b"x" * (COLS + 3))
    eq((s.row(0), s.row(1)), ("x" * COLS, ""))
    s = Screen(S("rmam") + S("smam") + b"x" * (COLS + 1))
    eq(s.row(1), "x")


def check_smir():
    eq(Screen(b"abc\r" + S("smir") + b"X").row(0), "Xabc")
    eq(Screen(b"abc\r" + S("smir") + S("rmir") + b"X").row(0), "Xbc")


def check_smacs():
    eq(Screen(S("smacs") + b"q").cells[0][0][0], 0x2500)
    eq(Screen(S("smacs") + S("rmacs") + b"q").cells[0][0][0], ord("q"))


def check_alt():
    s = Screen(b"main" + at(2, 3) + S("smcup") + b"alt")
    yes("altscreen" in s.modes, "smcup does not enter")
    eq((s.row(0), s.row(2)), ("", "   alt"))
    s = Screen(b"main" + at(2, 3) + S("smcup") + b"alt" + S("rmcup"))
    yes("altscreen" not in s.modes, "rmcup does not leave")
    eq((s.row(0), s.cy, s.cx), ("main", 2, 3))


def check_rs1():
    s = Screen(FOUR + b"\033[?1h\033[?2004h\033[?1000h\033[2;3r" + S("smul") + S("rs1") + b"x")
    eq(s.row(0), "x")
    eq((s.top, s.bot), (0, ROWS - 1))
    yes(not s.attr(0, 0, "underline"), "attributes stay")
    yes(not {"appcursor", "bracketed", "mousebtn"} & set(s.modes), f"modes stay: {s.modes}")
    yes("autowrap" in s.modes, "autowrap is not back on")


def check_is2():
    s = Screen(b"keep" + b"\033[2;3r\033[?1h\033=\033[?7l\033[4h\033[?25l" + S("is2"))
    eq(s.row(0), "keep")
    eq((s.top, s.bot), (0, ROWS - 1))
    yes(not {"appcursor", "appkeypad", "insert", "hidecursor"} & set(s.modes), f"modes stay: {s.modes}")
    yes("autowrap" in s.modes, "autowrap is not back on")


def check_tabs():
    eq(Screen(at(0, 3) + S("hts") + b"\r" + S("ht")).cx, 3)
    eq(Screen(S("tbc") + S("ht")).cx, COLS - 1)
    eq(Screen(at(0, 10) + S("cbt")).cx, 8)


def check_e3():
    eq(Screen(b"\n" * (ROWS + 5)).hist, 6)
    eq(Screen(b"\n" * (ROWS + 5) + S("E3")).hist, 0)


def check_ss():
    for n, style in ((0, "block"), (1, "block"), (2, "block"), (3, "underline"),
                     (4, "underline"), (5, "bar"), (6, "bar")):
        eq(Screen(S("Ss", n)).style, style)
    eq(Screen(S("Ss", 5) + S("Se")).style, "block")


def check_xm_mode():
    got = set(Screen(S("XM", 1)).modes)
    yes({"mousebtn", "mousesgr"} <= got, f"XM 1: {got}")
    got = set(Screen(S("XM", 1) + S("XM", 0)).modes)
    yes(not {"mousebtn", "mousesgr"} & got, f"XM 0: {got}")


def check_cpr():
    # u7 asks, u6 is the format of the answer
    s = Screen(at(2, 4) + S("u7"))
    eq(s.reply, b"\033[3;5R")
    eq(CAPS["u6"], b"\033[%i%d;%dR")


def check_da():
    # u9 asks, u8 is the format of the answer
    s = Screen(S("u9"))
    pat = CAPS["u8"].replace(b"%[;0123456789]", b"[;0-9]+").replace(b"[?", b"\\[\\?")
    yes(re.fullmatch(pat, s.reply), f"answer {s.reply!r} does not fit {CAPS['u8']!r}")


CHECKS = {
    "am": check_am, "bce": check_bce, "mir": check_mir, "msgr": check_msgr, "npc": check_npc,
    "xenl": check_xenl, "Tc": check_rgb("setrgbf", 1),
    "colors": check_color("setaf", 1), "pairs": lambda: eq(CAPS["pairs"], CAPS["colors"] ** 2),
    "cols": check_size("cols", "default_cols"), "lines": check_size("lines", "default_rows"),
    "it": check_ht_it, "ht": check_ht_it, "acsc": check_acsc, "bel": check_bel,
    "blink": sgr_on("blink", "blink"), "bold": sgr_on("bold", "bold"), "dim": sgr_on("dim", "dim"),
    "invis": sgr_on("invis", "invisible"), "rev": sgr_on("rev", "reverse"),
    "sitm": sgr_on_off("sitm", "ritm", "italic"), "ritm": sgr_on_off("sitm", "ritm", "italic"),
    "smso": sgr_on_off("smso", "rmso", "reverse"), "rmso": sgr_on_off("smso", "rmso", "reverse"),
    "smul": sgr_on_off("smul", "rmul", "underline"), "rmul": sgr_on_off("smul", "rmul", "underline"),
    "smxx": sgr_on_off("smxx", "rmxx", "strike"), "rmxx": sgr_on_off("smxx", "rmxx", "strike"),
    "cbt": check_tabs, "hts": check_tabs, "tbc": check_tabs,
    "civis": mode_set("civis", "hidecursor"), "cnorm": mode_set("cnorm", "hidecursor", off=True),
    "clear": check_clear, "cr": check_cr, "csr": check_csr,
    "cub": moved("cub", (2,), (1, 5), (1, 3)), "cub1": moved("cub1", (), (1, 5), (1, 4)),
    "cud": moved("cud", (2,), (1, 5), (3, 5)), "cud1": moved("cud1", (), (1, 5), (2, 5)),
    "cuf": moved("cuf", (3,), (1, 5), (1, 8)), "cuf1": moved("cuf1", (), (1, 5), (1, 6)),
    "cup": moved("cup", (2, 4), (0, 0), (2, 4)), "home": moved("home", (), (3, 5), (0, 0)),
    "cuu": moved("cuu", (2,), (3, 5), (1, 5)), "cuu1": moved("cuu1", (), (3, 5), (2, 5)),
    "hpa": moved("hpa", (7,), (2, 1), (2, 7)), "vpa": moved("vpa", (4,), (1, 3), (4, 3)),
    "dch": text_op("dch", (2,), b"abcdef", 1, "adef"), "dch1": text_op("dch1", (), b"abcdef", 1, "acdef"),
    "ech": text_op("ech", (2,), b"abcdef", 1, "a  def"), "ich": text_op("ich", (2,), b"abc", 1, "a  bc"),
    "dl": lines_op("dl", (2,), 1, ["a" * 10, "d" * 10, "", ""]),
    "dl1": lines_op("dl1", (), 1, ["a" * 10, "c" * 10, "d" * 10, ""]),
    "il": lines_op("il", (2,), 1, ["a" * 10, "", "", "b" * 10]),
    "il1": lines_op("il1", (), 1, ["a" * 10, "", "b" * 10, "c" * 10]),
    "ed": check_ed, "el": check_el, "el1": check_el1, "ind": check_ind, "indn": check_indn,
    "ri": check_ri, "rin": check_rin, "op": check_op, "sc": check_sc_rc, "rc": check_sc_rc,
    "is2": check_is2, "rs1": check_rs1, "rmacs": check_smacs, "smacs": check_smacs,
    "rmam": check_smam, "smam": check_smam, "rmcup": check_alt, "smcup": check_alt,
    "rmir": check_smir, "smir": check_smir, "smkx": mode_set("smkx", "appcursor", "appkeypad"),
    "rmkx": mode_set("rmkx", "appcursor", "appkeypad", off=True), "sgr": check_sgr, "sgr0": check_sgr0,
    "setaf": check_color("setaf", 1), "setab": check_color("setab", 2),
    "setrgbf": check_rgb("setrgbf", 1), "setrgbb": check_rgb("setrgbb", 2),
    "u6": check_cpr, "u7": check_cpr, "u8": check_da, "u9": check_da,
    "BE": mode_set("BE", "bracketed"), "BD": mode_set("BD", "bracketed", off=True),
    "E3": check_e3, "Se": check_ss, "Ss": check_ss, "XM": check_xm_mode,
}

# What mwt sends rather than what it takes, see tests/keys.py. PE and PS are the
# paste brackets, which tests/evil.sh paste_holds_until_return covers.
INPUT = re.compile(r"^(k.*|xm|PE|PS)$")


def main():
    fails = 0
    with tempfile.TemporaryDirectory() as d:
        CAPS.update(compile_entry(d))
    for name in sorted(CAPS):
        if INPUT.match(name):
            continue
        if name not in CHECKS:
            print(f"FAIL: {name}: in mwt.info, but no check here")
            fails += 1
            continue
        try:
            CHECKS[name]()
            print(f"ok: {name}")
        except (Failed, subprocess.CalledProcessError) as e:
            print(f"FAIL: {name}: {e}")
            fails += 1
    for name in sorted(set(CHECKS) - set(CAPS)):
        print(f"FAIL: {name}: checked here, but not in mwt.info")
        fails += 1
    print("PASS" if not fails else f"=== {fails} checks failed ===")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
