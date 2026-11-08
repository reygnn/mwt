#!/usr/bin/env python3
"""Matrix-Härtetest für mwt (läuft in jedem Terminal, zielt aber auf mwt).

Schreibt ohne curses rohe Escape-Sequenzen, so schnell die pty sie nimmt, in
sieben Phasen:

  regen       Vollbreite Katakana auf ungeraden und geraden Spalten (Hälften
              überlappen sich), Truecolor-Verlauf in jeder Schweifzelle,
              schwarzer Truecolor-Hintergrund über bce
  zerreissen  Breite Zeichen zerschneiden: Schreiben auf rechte Hälften, ICH,
              DCH, ECH, Insert-Modus, REP mit breiten Zeichen, Rand ohne Umbruch
  scrollen    Zufällige Scroll-Regionen, SU/SD, IL/DL, RI, Origin-Modus, bce
  attribute   Jede Zelle mit eigenem SGR (16/256/Truecolor, ; und :), dazu
              wechselnde Cursorformen
  splitter    regen und zerreissen in Stücken von 1-7 Bytes, mitten durch
              UTF-8 und Escape-Sequenzen
  muell       Ungültiges UTF-8, C1, abgebrochene OSC/DCS, riesige Parameter,
              Kombinationszeichen, ZWJ, Tabs, DEC-Grafik
  mix         alles durcheinander

Während des Laufs misst eine DA-Anfrage (CSI c) alle 0,2 s, wie weit mwt der
Ausgabe hinterherhinkt. Vorher und nachher prüfen Cursor-Abfragen (CPR), ob mwt
noch richtig rechnet. Fenster ziehen oder Ctrl+Shift+Plus/Minus während des
Laufs testet Resizes unter Last. q beendet, Ctrl+C auch.

  python3 matrix_haerte.py                  # 70 s, alle Phasen
  python3 matrix_haerte.py -s 20 -p regen   # nur eine Phase
  python3 matrix_haerte.py --fps 30         # gebremst, zum Anschauen
  python3 matrix_haerte.py --dump -g 80x24 > out.bin   # Bytes für cat, ohne Abfragen
  time cat out.bin                                      # mwts eigener Durchsatz
  python3 matrix_haerte.py --dump-checks -g 80x24 > c.bin  # nur die Prüfungen

Exit: 0 alles gut, 1 eine Prüfung falsch, 2 mwt hängt (keine Antwort/kein
Schreiben innerhalb von --hang Sekunden).
"""
import argparse
import os
import random
import re
import select
import signal
import sys
import termios
import time

ESC = "\x1b"
CSI = ESC + "["
KATA = [chr(c) for c in range(0x30A1, 0x30F7)]       # vollbreit, 2 Zellen
HALB = [chr(c) for c in range(0xFF66, 0xFF9E)]       # halbbreit, 1 Zelle
ASCII = list("0123456789Z:.=*+-<>|")
EMOJI = ["🙂", "🚀", "🔥", "👾", "🐍", "漢", "字"]   # 2 Zellen
# Zustand, aus dem jede Phase startet und in dem das Skript endet. SI und
# ESC ( B holen den Zeichensatz zurück, CAN bricht eine offene Sequenz ab.
RESET = "\x18\x0f" + ESC + "(B" + CSI + "0m" + CSI + "r" + CSI + "?6l" + CSI + "?7h" \
    + CSI + "4l" + CSI + "?25l" + CSI + "0 q"


def cup(y, x):
    return f"{CSI}{y + 1};{x + 1}H"


def default_tabs(cols):
    return CSI + "3g" + "".join(f"{CSI}1;{c}H{ESC}H" for c in range(9, cols + 1, 8)) + CSI + "H"


def rgb(r, g, b, colon=False, bg=False):
    k = 48 if bg else 38
    return f"{k}:2::{r}:{g}:{b}" if colon else f"{k};2;{r};{g};{b}"


# ---- Phasen --------------------------------------------------------------

class Phase:
    split = False

    def init(self, cols, rows, rnd):
        self.c, self.r, self.rnd = cols, rows, rnd
        return CSI + "0m" + CSI + "2J"


class Regen(Phase):
    name = "regen"
    BLACK = CSI + "0;" + rgb(0, 0, 0, bg=True) + "m"

    def init(self, cols, rows, rnd):
        self.c, self.r, self.rnd = cols, rows, rnd
        self.drops = [self.new(True) for _ in range(max(1, cols * 2 // 3))]
        return self.BLACK + CSI + "2J"   # bce: der Hintergrund kommt aus dem SGR

    def new(self, anywhere=False):
        rnd = self.rnd
        wide = self.c >= 2 and rnd.random() < 0.6
        ln = rnd.randint(4, max(5, self.r))
        return {
            "x": rnd.randrange(0, self.c - (1 if wide else 0)),  # auch ungerade
            "y": rnd.uniform(-self.r, self.r) if anywhere else rnd.uniform(-ln, 0),
            "v": rnd.uniform(0.3, 1.6), "ln": ln, "wide": wide,
            "colon": rnd.random() < 0.3, "g": {},
        }

    def frame(self):
        out, rnd = [], self.rnd
        for i, d in enumerate(self.drops):
            old = int(d["y"])
            d["y"] += d["v"]
            head = int(d["y"])
            gap = "  " if d["wide"] else " "
            for yy in range(old - d["ln"], head - d["ln"]):
                if 0 <= yy < self.r:
                    out.append(cup(yy, d["x"]) + self.BLACK + gap)
                    d["g"].pop(yy, None)
            pool = KATA if d["wide"] else HALB + ASCII
            for k in range(d["ln"]):
                yy = head - k
                if not 0 <= yy < self.r:
                    continue
                if k == 0 or yy not in d["g"] or rnd.random() < 0.06:
                    d["g"][yy] = rnd.choice(pool)
                if k == 0:
                    sgr = "1;" + rgb(235, 255, 235, d["colon"])
                else:
                    t = 1 - k / d["ln"]
                    sgr = rgb(int(40 * t * t), int(30 + 225 * t), int(60 * t * t), d["colon"])
                    if k == 1:
                        sgr = "1;" + sgr
                out.append(f"{cup(yy, d['x'])}{CSI}0;{sgr};{rgb(0, 0, 0, bg=True)}m{d['g'][yy]}")
            if head - d["ln"] >= self.r:
                self.drops[i] = self.new()
        return "".join(out)


class Zerreissen(Phase):
    name = "zerreissen"

    def init(self, cols, rows, rnd):
        self.c, self.r, self.rnd = cols, rows, rnd
        out = [CSI + "0m" + CSI + "2J"]
        for y in range(rows):
            out.append(f"{cup(y, y % 2)}{CSI}0;38;5;{rnd.randrange(16, 232)}m")
            out.append("".join(rnd.choice(KATA) for _ in range((cols - y % 2) // 2)))
        return "".join(out)

    def frame(self):
        rnd, c, r, out = self.rnd, self.c, self.r, []
        for _ in range(rnd.randint(100, 300)):
            y, x, n = rnd.randrange(r), rnd.randrange(c), rnd.randint(1, 6)
            out.append(f"{cup(y, x)}{CSI}0;38;5;{rnd.randrange(256)}m")
            op = rnd.randrange(10)
            if op == 0:
                out.append(rnd.choice(ASCII))                 # auf eine Hälfte
            elif op == 1:
                out.append(rnd.choice(KATA + EMOJI))           # am Rand: Umbruch
            elif op == 2:
                out.append(f"{CSI}{n}@")
            elif op == 3:
                out.append(f"{CSI}{n}P")
            elif op == 4:
                out.append(f"{CSI}{n}X")
            elif op == 5:
                out.append(f"{CSI}4h" + "".join(rnd.choice(KATA) for _ in range(n)) + f"{CSI}4l")
            elif op == 6:
                out.append(rnd.choice(KATA) + f"{CSI}{n}b")
            elif op == 7:
                out.append(f"{CSI}{rnd.randrange(3)}K")
            elif op == 8:
                out.append(f"{CSI}?7l{cup(y, max(0, c - n))}" + "".join(rnd.choice(KATA) for _ in range(n + 2)) + f"{CSI}?7h")
            else:
                out.append(rnd.choice(KATA) + "\b" + rnd.choice(ASCII))   # BS in eine Hälfte
        return "".join(out)


class Scrollen(Phase):
    name = "scrollen"

    def init(self, cols, rows, rnd):
        self.c, self.r, self.rnd, self.n = cols, rows, rnd, 0
        return CSI + "0m" + CSI + "2J" + "".join(f"{cup(y, 0)}Zeile {y}" for y in range(rows))

    def line(self):
        self.n += 1
        s = f"{self.n:06d} " + "".join(self.rnd.choice(KATA + HALB) for _ in range(self.c // 3))
        return s

    def frame(self):
        rnd, r = self.rnd, self.r
        if r < 2:
            return ""
        top = rnd.randrange(0, r - 1)
        bot = rnd.randrange(top + 1, r)
        out = [f"{CSI}{top + 1};{bot + 1}r{CSI}0;48;5;{rnd.randrange(256)};38;5;{rnd.randrange(256)}m"]
        for _ in range(rnd.randint(8, 25)):
            k = rnd.randint(1, bot - top + 2)
            op = rnd.randrange(6)
            if op == 0:
                out.append(cup(bot, 0) + "".join(self.line() + "\r\n" for _ in range(k)))
            elif op == 1:
                out.append(cup(top, 0) + (ESC + "M") * k + self.line())
            elif op == 2:
                out.append(f"{CSI}{k}S")
            elif op == 3:
                out.append(f"{CSI}{k}T")
            elif op == 4:
                out.append(cup(rnd.randint(top, bot), 0) + f"{CSI}{k}{rnd.choice('LM')}")
            else:
                out.append(f"{CSI}?6h{CSI}{k};1H{self.line()}{CSI}?6l")
        out.append(CSI + "r" + CSI + "0m")
        return "".join(out)


def rnd_color(rnd, bg):
    b = 10 if bg else 0
    k = rnd.randrange(6)
    if k == 0:
        return str(30 + b + rnd.randrange(8))
    if k == 1:
        return str(90 + b + rnd.randrange(8))
    if k == 2:
        return f"{38 + b};5;{rnd.randrange(256)}"
    if k == 3:
        return f"{38 + b}:5:{rnd.randrange(256)}"
    return rgb(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), k == 5, bg)


class Attribute(Phase):
    name = "attribute"

    def frame(self):
        rnd, out = self.rnd, []
        for y in range(self.r):
            out.append(cup(y, 0))
            x = 0
            while x < self.c:
                p = ["0"] + [str(a) for a, w in ((1, .2), (2, .1), (3, .2), (4, .2), (5, .05),
                                                (7, .1), (8, .02), (9, .15)) if rnd.random() < w]
                p.append(rnd_color(rnd, False))
                if rnd.random() < 0.5:
                    p.append(rnd_color(rnd, True))
                wide = x < self.c - 1 and rnd.random() < 0.3
                ch = rnd.choice(KATA + EMOJI) if wide else rnd.choice(ASCII + HALB)
                out.append(f"{CSI}{';'.join(p)}m{ch}")
                x += 2 if wide else 1
        out.append(f"{CSI}0m{CSI}{rnd.randrange(7)} q{CSI}?25h{cup(rnd.randrange(self.r), rnd.randrange(self.c))}")
        return "".join(out)


class Splitter(Phase):
    name = "splitter"
    split = True

    def init(self, cols, rows, rnd):
        self.a, self.b, self.n = Regen(), Zerreissen(), 0
        return self.b.init(cols, rows, rnd) + self.a.init(cols, rows, rnd)

    def frame(self):
        self.n += 1
        return (self.a if self.n % 2 else self.b).frame()


class Muell(Phase):
    name = "muell"

    POOL = bytes(b for b in range(256) if b not in (0x1b, 0x07, 0x9b))
    STATIC = [
        b"\xff", b"\xfe\xfe", b"\xc3", b"\xe3\x82", b"\xf0\x9f\x98", b"\xc0\xaf",
        b"\xed\xa0\x80", b"\xf4\x90\x80\x80", b"\x9b2J", "\u0085\u009b2J".encode(),
        b"\x1b[99999999999;99999999999H", b"\x1b[65535b", b"\x1b[65535@", b"\x1b[65535P",
        b"\x1b[65535L", b"\x1b[65535M", b"\x1b[65535S", b"\x1b[65535T", b"\x1b[65535X",
        b"\x1b[0;0r", b"\x1b[5;2r", b"\x1b[:::;;;::m", b"\x1b[38;2m", b"\x1b[38;5m",
        b"\x1b[38:2:1:2:3:4:5:6m", b"\x1b[48;2;300;-1;99999m", b"\x1b[?6h\x1b[99;99H",
        "👨\u200d👩\u200d👧 ❤\ufe0f 🇩🇪 ア\u3099".encode(),
        b"\x1b(0lqqqqqk\x0ex\x0f\x1b(B", b"\x0e" + b"lqk" * 10,
        b"\x1b[3g\x1bH\t\t\t\x1b[Z\x1b[5I\x1b[9Z", "アイ\b\bx\b\b\by".encode(),
        b"\x1b[" + b";".join(b"%d" % i for i in range(40)) + b" q",
        b"\x1b[?1049h", b"\x1b#8", b"\x1b[" + b"1;" * 30 + b"1H",
        b"\x1b\x1b\x1b[[[", b"\x1b[?\x1b[>\x1b[=", b"\x1b[1\x00;\x7f2H",
    ]

    # Abfragen: mwt antwortet. Live liest das Skript die Antworten; bei --dump
    # liest sie niemand, dann landen sie in der Eingabe, und mit ECHO gibt sie
    # die pty mitten in die Ausgabe zurück. Ein Echo-Rest "c" hinter einem ESC
    # ergibt ESC c (RIS): Reset, raus aus dem Alternate Screen.
    QUERIES = [b"\x1b[?1;6;7;25;1049;2004;9999$p", b"\x1b[>c", b"\x1b[5n", b"\x1b[18t"]
    queries = True

    def junk(self):
        rnd = self.rnd
        items = self.STATIC + self.QUERIES if self.queries else self.STATIC
        k = rnd.randrange(len(items) + 6)
        if k < len(items):
            return items[k]
        k -= len(items)
        n = rnd.randint(1, 3000)
        if k == 0:
            return b"\x1b]9999;" + b"x" * n + b"\x18"        # unbekannte OSC, CAN
        if k == 1:
            return b"\x1b]9999;" + b"y" * n                   # ohne Ende: bis zum nächsten ESC
        if k == 2:
            return b"\x1bP1;2|" + b"z" * n + b"\x1b\\"     # DCS
        if k == 3:
            return b"\x1b[" + b";".join(b"%d" % rnd.randrange(10 ** 9)
                                        for _ in range(rnd.randint(17, 400))) + b"m"
        if k == 4:
            return ("a" + "\u0301\u0308\u0323" * rnd.randint(1, 50)).encode()
        return bytes(rnd.choices(self.POOL, k=rnd.randint(1, 200)))

    def frame(self):
        rnd, out = self.rnd, []
        for _ in range(rnd.randint(20, 60)):
            out.append(cup(rnd.randrange(self.r), rnd.randrange(self.c)).encode())
            out.append(self.junk())
            out.append(RESET.encode())
        return b"".join(out)


class Mix(Phase):
    name = "mix"
    queries = True

    def init(self, cols, rows, rnd):
        m = Muell()
        m.queries = self.queries
        self.subs = [Regen(), Zerreissen(), Scrollen(), Attribute(), m]
        s = "".join(p.init(cols, rows, rnd) for p in self.subs)
        self.rnd = rnd
        return s

    def frame(self):
        f = self.rnd.choice(self.subs).frame()
        return f if isinstance(f, bytes) else f.encode()


PHASES = [Regen, Zerreissen, Scrollen, Attribute, Splitter, Muell, Mix]


# ---- Prüfungen -----------------------------------------------------------

def checks(cols, rows):
    """(Name, Bytes, erwartete CPR-Antwort (Zeile, Spalte)) wie xterm."""
    if cols < 20 or rows < 6:
        return []
    c = cols
    return [
        ("drei breite Zeichen", "アイウ", (1, 7)),
        ("Kombinationszeichen, ZWJ verworfen", "é́x‍", (1, 3)),
        ("REP mit breitem Zeichen", f"ア{CSI}4b", (1, 11)),
        ("breites Zeichen in letzter Spalte bricht um", f"{cup(1, c - 1)}ア", (3, 3)),
        ("ohne Autowrap", f"{CSI}?7l{cup(1, 0)}{'x' * (c + 5)}{cup(2, c - 1)}ア{CSI}?7h", (3, c)),
        ("ungültiges UTF-8 wird je ein U+FFFD", b"a\xffb\xe3\x82x", (1, 6)),
        ("C1 in UTF-8 ignoriert, 2J bleibt Text", "ab\u0085\u009b2Jcd", (1, 7)),
        ("abgebrochene OSC", f"{ESC}]9999;blah{cup(3, 5)}", (4, 6)),
        ("riesige Parameter", f"{CSI}99999;99999H", (rows, c)),
        ("CPR relativ im Origin-Modus", f"{CSI}3;5r{CSI}?6h{CSI}2;4H", (2, 4)),
        ("Insert-Modus mit breitem Zeichen", f"abc{CSI}1G{CSI}4hア{CSI}4l", (1, 3)),
        ("Tabstopps", f"{CSI}3g{cup(0, 4)}{ESC}H{cup(0, 12)}{ESC}H{CSI}H\t\t", (1, 13)),
        ("Tab ohne Stopp bis zum Rand", f"{CSI}3g{CSI}H\t", (1, c)),
        ("Backspace in breites Zeichen", "アイ\b\b", (1, 3)),
        ("Bildschirm voll, Scrollen", f"{cup(rows - 1, 0)}\n\n\nx", (rows, 2)),
    ]


def prep(cols):
    return RESET + CSI + "2J" + default_tabs(cols) + CSI + "H"


# ---- Terminal -----------------------------------------------------------

CPR = re.compile(rb"\x1b\[(\d+);(\d+)R")
DA1 = re.compile(rb"\x1b\[\?[\d;]*c")
ANY = re.compile(rb"\x1b(?:\[[\x20-\x3f]*[\x40-\x7e]|\][^\x07\x1b]*(?:\x07|\x1b\\)|P[^\x1b]*\x1b\\)")


class Hang(Exception):
    pass


class Term:
    def __init__(self, fin, fout, hang):
        self.fin, self.fout, self.hang = fin, fout, hang
        self.inbuf = b""
        self.quit = False
        self.cprs = []
        self.da_sent = None
        self.da_lat = []
        self.bytes = 0

    def write(self, data, split=False, rnd=None):
        if isinstance(data, str):
            data = data.encode()
        self.bytes += len(data)
        mv, i = memoryview(data), 0
        while i < len(data):
            n = rnd.randint(1, 7) if split else 4096
            _, w, _ = select.select([], [self.fout], [], self.hang)
            if not w:
                raise Hang(f"mwt nimmt seit {self.hang} s keine Ausgabe mehr an")
            i += os.write(self.fout, mv[i:i + n])
            if not split or i % 512 < 8:
                self.poll()

    def poll(self, timeout=0):
        r, _, _ = select.select([self.fin], [], [], timeout)
        if not r:
            return
        self.inbuf += os.read(self.fin, 65536)
        for m in CPR.finditer(self.inbuf):
            self.cprs.append((int(m[1]), int(m[2])))
        if DA1.search(self.inbuf) and self.da_sent is not None:
            self.da_lat.append(time.monotonic() - self.da_sent)
            self.da_sent = None
        keys = ANY.sub(b"", self.inbuf)
        if b"q" in keys or b"Q" in keys or b"\x03" in keys:
            self.quit = True
        # unvollständige Sequenz am Ende aufheben
        cut = self.inbuf.rfind(b"\x1b")
        self.inbuf = self.inbuf[cut:] if cut >= 0 and not ANY.match(self.inbuf, cut) and len(self.inbuf) - cut < 64 else b""

    def ping(self):
        now = time.monotonic()
        if self.da_sent is None:
            self.da_sent = now
            self.write(CSI + "c")
        elif now - self.da_sent > self.hang:
            raise Hang(f"keine Antwort auf DA seit {self.hang} s")

    def cpr(self, timeout=3.0):
        self.cprs.clear()
        self.write(CSI + "6n")
        end = time.monotonic() + timeout
        while not self.cprs and time.monotonic() < end:
            self.poll(0.05)
        return self.cprs[0] if self.cprs else None


def run_checks(term, fout, resized):
    """Wiederholt die Runde, wenn währenddessen ein Resize kam: dann stimmen
    die Erwartungen nicht mehr."""
    for _ in range(5):
        resized[0] = False
        cols, rows = os.get_terminal_size(fout)
        res = []
        for name, seq, exp in checks(cols, rows):
            term.write(prep(cols))
            term.write(seq)
            got = term.cpr()
            if got is None:
                raise Hang("keine Antwort auf die Cursor-Abfrage (CSI 6n)")
            res.append((name, exp, got))
        term.write(prep(cols))
        if not resized[0]:
            break
    return res


# ---- Ablauf ---------------------------------------------------------------

def status(term, cols, rows, text):
    if rows < 3 or cols < 10:
        return
    text = text[:cols - 1].ljust(cols - 1)
    term.write(f"{CSI}s{cup(rows - 1, 0)}{CSI}0;7m{text}{CSI}0m{CSI}u")


def live(args):
    fin, fout = sys.stdin.fileno(), sys.stdout.fileno()
    if not (os.isatty(fin) and os.isatty(fout)):
        sys.exit("Braucht ein Terminal (oder --dump).")
    saved = termios.tcgetattr(fin)
    new = termios.tcgetattr(fin)
    new[3] &= ~(termios.ICANON | termios.ECHO | termios.ISIG)
    new[1] &= ~termios.OPOST                  # \n bleibt \n, sonst kein echtes LF
    new[6][termios.VMIN], new[6][termios.VTIME] = 0, 0
    termios.tcsetattr(fin, termios.TCSAFLUSH, new)

    resized = [False]
    signal.signal(signal.SIGWINCH, lambda *a: resized.__setitem__(0, True))
    rnd = random.Random(args.seed)
    term = Term(fin, fout, args.hang)
    report, before, after, hang, resizes = [], [], [], None, 0
    names = [p.name for p in PHASES]
    todo = [p for p in PHASES if not args.phase or p.name in args.phase]
    try:
        term.write(CSI + "?1049h" + RESET)
        before = run_checks(term, fout, resized)
        cols, rows = os.get_terminal_size(fout)
        share = args.seconds / len(todo)
        for P in todo:
            if term.quit:
                break
            ph = P()
            term.write(RESET + ph.init(cols, rows, rnd))
            b0, t0, frames, lat0 = term.bytes, time.monotonic(), 0, len(term.da_lat)
            last_stat = last_ping = 0.0
            while time.monotonic() - t0 < share and not term.quit:
                if resized[0]:
                    resized[0] = False
                    resizes += 1
                    cols, rows = os.get_terminal_size(fout)
                    term.write(RESET + ph.init(cols, rows, rnd))
                ft = time.monotonic()
                term.write(ph.frame(), ph.split, rnd)
                frames += 1
                now = time.monotonic()
                if now - last_ping > 0.2:
                    last_ping = now
                    term.ping()
                term.poll()
                if now - last_stat > 0.25:
                    last_stat = now
                    dt = now - t0
                    lat = term.da_lat[-1] * 1000 if len(term.da_lat) > lat0 else float("nan")
                    status(term, cols, rows,
                           f" {P.name} [{names.index(P.name) + 1}/{len(names)}]  "
                           f"{(term.bytes - b0) / dt / 1e6:5.2f} MB/s  {frames / dt:6.1f} Bilder/s  "
                           f"DA {lat:6.0f} ms  {cols}x{rows}  q=Ende ")
                if args.fps:
                    time.sleep(max(0, 1 / args.fps - (time.monotonic() - ft)))
            dt = time.monotonic() - t0
            lats = term.da_lat[lat0:]
            report.append((P.name, term.bytes - b0, dt, frames,
                           max(lats) if lats else None, sum(lats) / len(lats) if lats else None))
        # Rückstand abwarten, dann prüfen
        term.write(RESET)
        term.da_sent = None
        term.ping()
        end = time.monotonic() + args.hang
        while term.da_sent is not None and time.monotonic() < end:
            term.poll(0.05)
        if term.da_sent is not None:
            raise Hang("nach dem Lauf keine Antwort auf DA")
        after = run_checks(term, fout, resized)
    except Hang as e:
        hang = str(e)
    except KeyboardInterrupt:
        pass
    finally:
        if hang is None:
            cols, rows = os.get_terminal_size(fout)
            term.write(RESET + default_tabs(cols) + CSI + "?25h" + CSI + "?1049l")
        termios.tcsetattr(fin, termios.TCSAFLUSH, saved)

    lines = ["Matrix-Härtetest für mwt", ""]
    lines.append(f"{'Phase':<12}{'MB':>8}{'MB/s':>8}{'Bilder/s':>10}{'DA max':>10}{'DA Ø':>9}")
    for name, b, dt, fr, mx, av in report:
        f = lambda v: f"{v * 1000:7.0f}ms" if v is not None else "      - "
        lines.append(f"{name:<12}{b / 1e6:8.1f}{b / dt / 1e6:8.2f}{fr / dt:10.1f}{f(mx):>10}{f(av):>9}")
    lines.append(f"Resizes während des Laufs: {resizes}")
    failed = 0
    for title, res in (("vorher", before), ("nachher", after)):
        if not res:
            continue
        bad = [r for r in res if r[1] != r[2]]
        failed += len(bad)
        lines.append(f"Prüfungen {title}: {len(res) - len(bad)}/{len(res)} richtig")
        for name, exp, got in bad:
            lines.append(f"  FALSCH  {name}: erwartet {exp}, bekommen {got}")
    if hang:
        lines.append(f"HÄNGT: {hang}")
    text = "\n".join(lines) + "\n"
    if hang:   # aufs Terminal zu schreiben könnte selbst hängen
        with open("matrix_haerte.log", "w") as fh:
            fh.write(text)
        sys.exit(2)
    sys.stdout.write(text)
    if args.log:
        with open(args.log, "w") as fh:
            fh.write(text)
    sys.exit(1 if failed else 0)


def dump(args):
    cols, rows = args.geometry
    rnd = random.Random(args.seed)
    out = sys.stdout.buffer
    if args.dump_checks:
        exp = []
        for name, seq, e in checks(cols, rows):
            out.write(prep(cols).encode())
            out.write(seq if isinstance(seq, bytes) else seq.encode())
            out.write(b"\x1b[6n")
            exp.append(e)
        out.write(prep(cols).encode())
        sys.stderr.write("".join(f"\x1b[{y};{x}R" for y, x in exp).encode().hex() + "\n")
        return
    out.write((CSI + "?1049h" + RESET).encode())
    for P in PHASES:
        if args.phase and P.name not in args.phase:
            continue
        ph = P()
        ph.queries = args.queries
        out.write((RESET + ph.init(cols, rows, rnd)).encode())
        for _ in range(args.frames):
            f = ph.frame()
            out.write(f if isinstance(f, bytes) else f.encode())
    out.write((RESET + default_tabs(cols) + CSI + "?25h" + CSI + "?1049l").encode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--seconds", type=float, default=70, help="Gesamtdauer (Standard 70)")
    ap.add_argument("-p", "--phase", action="append", choices=[p.name for p in PHASES],
                    help="nur diese Phase(n)")
    ap.add_argument("--fps", type=float, default=0, help="Bilder pro Sekunde, 0 = ungebremst")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--hang", type=float, default=10, help="Sekunden ohne Antwort bis 'hängt'")
    ap.add_argument("--log", help="Bericht zusätzlich in diese Datei")
    ap.add_argument("--dump", action="store_true", help="Bytes auf stdout statt live")
    ap.add_argument("--dump-checks", action="store_true", help="nur die Prüfungen; erwartete Antworten hex auf stderr")
    ap.add_argument("--queries", action="store_true",
                    help="bei --dump auch Abfragen (DA2, DSR, DECRQM); nur für vtdump/replay, nicht für cat")
    ap.add_argument("--frames", type=int, default=40, help="Bilder pro Phase bei --dump")
    ap.add_argument("-g", "--geometry", default="80x24", help="SPALTENxZEILEN bei --dump")
    args = ap.parse_args()
    args.geometry = tuple(int(v) for v in args.geometry.lower().split("x"))
    if args.dump or args.dump_checks:
        dump(args)
    else:
        live(args)


if __name__ == "__main__":
    main()
