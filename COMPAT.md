# Compatibility

What mwt does of the VT100, and of what came after it. The reference for the VT100 is
the [VT100 User Guide](https://vt100.net/docs/vt100-ug/chapter3.html) (EK-VT100-UG-003),
chapter 3; for the VT102 its user guide; for everything later
[xterm's ctlseqs](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html). Why things
are left out is in CAVEATS.md.

✅ as the reference · 🟡 partly, see the note · ❌ not done; the sequence is parsed and ignored

## Summary

| Group | ✅ | 🟡 | ❌ |
|---|---:|---:|---:|
| VT100 control characters | 9 | 2 | 1 |
| VT100 escape and control sequences | 25 | 3 | 5 |
| VT100 modes | 4 | 0 | 6 |
| VT100 keyboard | 4 | 1 | 2 |
| VT52 mode | 0 | 0 | 1 |
| VT102 additions | 4 | 0 | 1 |
| **VT100 and VT102 together** | **46** | **6** | **16** |
| Beyond the VT100 | 34 | 13 | 12 |


## VT100

### Control characters

| Code | Name | mwt | Note |
|---|---|:-:|---|
| 0x00 | NUL | ✅ | ignored, as on the VT100 |
| 0x05 | ENQ | ❌ | no answerback message |
| 0x07 | BEL | 🟡 | without the focus the window asks for attention through xdg-activation, once until it has the focus again; no sound, no flash (CAVEATS.md) |
| 0x08 | BS | ✅ | stops at the first column |
| 0x09 | HT | ✅ | |
| 0x0A–0x0C | LF, VT, FF | ✅ | with LNM also a carriage return |
| 0x0D | CR | ✅ | |
| 0x0E, 0x0F | SO, SI | ✅ | G1 or G0 into GL |
| 0x11, 0x13 | XON, XOFF | ✅ | the pty's line discipline handles them, not mwt |
| 0x18, 0x1A | CAN, SUB | 🟡 | end a sequence; the VT100 also shows its error character (a checkerboard), and xterm prints ␦ for SUB; mwt shows nothing |
| 0x1B | ESC | ✅ | |
| 0x7F | DEL | ✅ | ignored, also within an escape sequence |

### Escape and control sequences

| Sequence | Name | mwt | Note |
|---|---|:-:|---|
| `CSI Pn A` | CUU, cursor up | ✅ | stops at the top margin |
| `CSI Pn B` | CUD, cursor down | ✅ | stops at the bottom margin |
| `CSI Pn C` | CUF, cursor forward | ✅ | |
| `CSI Pn D` | CUB, cursor backward | ✅ | |
| `CSI Pl ; Pc H` | CUP, cursor position | ✅ | relative to the margins in origin mode |
| `CSI Pl ; Pc f` | HVP | ✅ | as CUP |
| `ESC D` | IND, index | ✅ | |
| `ESC E` | NEL, next line | ✅ | |
| `ESC M` | RI, reverse index | ✅ | |
| `ESC 7` | DECSC, save cursor | ✅ | position, attributes, G0, G1, GL and origin mode, one set for each screen |
| `ESC 8` | DECRC, restore cursor | ✅ | |
| `CSI Pt ; Pb r` | DECSTBM, scroll margins | ✅ | |
| `CSI Ps J` | ED, erase in display | ✅ | 0, 1, 2; for 3 see below |
| `CSI Ps K` | EL, erase in line | ✅ | 0, 1, 2 |
| `ESC H` | HTS, set tab | ✅ | |
| `CSI g`, `CSI 3 g` | TBC, clear tabs | ✅ | |
| `CSI Ps ; … m` | SGR 0, 1, 4, 7 | ✅ | normal, bold, underline, reverse |
| `CSI 5 m` | SGR 5, blink | 🟡 | stored, and DECRQCRA counts it, but drawn steady |
| `CSI Ps h`, `CSI Ps l` | SM, RM | ✅ | see Modes |
| `ESC ( C`, `ESC ) C` | SCS, G0 and G1 | 🟡 | `B` (US) and `0` (special graphics) as on the VT100. `A` (UK) is US ASCII, so `#` stays `#` and is not `£`. `1` and `2` (the alternate character ROM) are US ASCII too; CAVEATS.md counts them among the national character sets |
| `CSI c`, `CSI 0 c` | DA | ✅ | answers `CSI ? 62 ; 22 c` (a VT220 with color); a VT100 answers `CSI ? 1 ; 2 c` |
| `ESC Z` | DECID | ✅ | same answer as DA |
| `CSI 5 n` | DSR, status | ✅ | `CSI 0 n` |
| `CSI 6 n` | DSR, CPR | ✅ | relative to the margins in origin mode |
| `CSI x`, `CSI 1 x` | DECREQTPARM | ❌ | no DECREPTPARM answer |
| `CSI 2 ; Ps y` | DECTST | ❌ | no self test |
| `CSI Ps q` | DECLL | ❌ | no keyboard LEDs |
| `ESC # 3`, `ESC # 4` | DECDHL | ❌ | no double height |
| `ESC # 6` | DECDWL | ❌ | no double width |
| `ESC # 5` | DECSWL | ✅ | nothing to do, since every line is single width |
| `ESC # 8` | DECALN | ✅ | also resets the margins, origin mode and the attributes, and homes the cursor, as xterm |
| `ESC =`, `ESC >` | DECKPAM, DECKPNM | 🟡 | DECRQM reports the mode, but the keypad sends the same in both (CAVEATS.md, Keypad mode) |
| `ESC c` | RIS | ✅ | |

### Modes

| Sequence | Name | mwt | Note |
|---|---|:-:|---|
| `CSI 20 h` | LNM, newline mode | ✅ | LF also returns, and Return sends CR LF |
| `CSI ? 1 h` | DECCKM, cursor keys | ✅ | |
| `CSI ? 2 l` | DECANM, VT52 mode | ❌ | no VT52 mode; DECRQM answers 0 |
| `CSI ? 3 h` | DECCOLM, 132 columns | ❌ | the column count stays; DECRQM answers 4 |
| `CSI ? 4 h` | DECSCLM, smooth scroll | ❌ | always jump scroll; DECRQM answers 4 |
| `CSI ? 5 h` | DECSCNM, reverse screen | ❌ | DECRQM answers 4 |
| `CSI ? 6 h` | DECOM, origin mode | ✅ | |
| `CSI ? 7 h` | DECAWM, autowrap | ✅ | |
| `CSI ? 8 h` | DECARM, autorepeat | ❌ | key repeat comes from the compositor; DECRQM answers 0 |
| `CSI ? 9 h` | DECINLM, interlace | ❌ | DECRQM answers 4; xterm's X10 mouse on the same number is not done either |

### Keyboard

| Key | VT100 sends | mwt | Note |
|---|---|:-:|---|
| Cursor keys | `ESC [ A`, or `ESC O A` with DECCKM | ✅ | with modifiers `CSI 1 ; m A`, as xterm; mwt.info declares them (`kUP5` and the like) |
| PF1–PF4 | `ESC O P` to `ESC O S` | ✅ | on F1 to F4 |
| Return | CR, or CR LF with LNM | ✅ | |
| Backspace | BS (0x08) | 🟡 | DEL (0x7F), as xterm and `kbs` in mwt.info; Ctrl+Backspace sends BS |
| Ctrl with a letter or `[ \ ] ^ _` | C0 controls | ✅ | |
| Keypad in application mode | `ESC O p` to `ESC O y`, `ESC O m`, `l`, `n`, `M` | ❌ | always the digits and CR (CAVEATS.md, Keypad mode) |
| Answerback | the stored message on ENQ or Ctrl+Break | ❌ | |

### VT52 mode

| Sequence | Name | mwt | Note |
|---|---|:-:|---|
| `ESC A` to `ESC Z`, `ESC Y l c`, `ESC <` | VT52 | ❌ | left out on purpose (README) |

### VT102 additions

| Sequence | Name | mwt | Note |
|---|---|:-:|---|
| `CSI Pn P` | DCH, delete characters | ✅ | |
| `CSI Pn L` | IL, insert lines | ✅ | only inside the scroll margins |
| `CSI Pn M` | DL, delete lines | ✅ | only inside the scroll margins |
| `CSI 4 h` | IRM, insert mode | ✅ | |
| `CSI Ps i`, `CSI ? Ps i` | MC, media copy | ❌ | no printer |


## Beyond the VT100

### Cursor, editing and scrolling

| Sequence | Name | From | mwt | Note |
|---|---|---|:-:|---|
| `CSI Pn @` | ICH, insert characters | VT220 | ✅ | |
| `CSI Pn X` | ECH, erase characters | VT220 | ✅ | |
| `CSI Pn E`, `CSI Pn F` | CNL, CPL | ECMA-48 | ✅ | |
| ``CSI Pn G``, ``CSI Pn ` `` | CHA, HPA | ECMA-48 | ✅ | |
| `CSI Pn a`, `CSI Pn e` | HPR, VPR | ECMA-48 | ✅ | |
| `CSI Pn d` | VPA | ECMA-48 | ✅ | |
| `CSI Pn I`, `CSI Pn Z` | CHT, CBT | ECMA-48 | ✅ | |
| `CSI Pn S`, `CSI Pn T` | SU, SD | ECMA-48 | ✅ | inside the scroll margins |
| `CSI Pn b` | REP | ECMA-48 | ✅ | the screen as after all the copies; past a screenful and a row the history gets fewer lines. What has no width is not repeated, as xterm |
| `CSI 3 J` | ED 3, erase scrollback | xterm | ✅ | |
| `CSI s`, `CSI u` | SCOSC, SCORC | SCO | ✅ | as DECSC and DECRC |
| `CSI ! p` | DECSTR, soft reset | VT220 | ✅ | resets all of SGR, as DEC has it (xterm keeps faint, crossed out and double underline), and the cursor style |
| `CSI ? 69 h`, `CSI Pl ; Pr s` | DECLRMM, DECSLRM, left and right margins | VT420 | ❌ | |
| `CSI ? 45 h` | reverse wraparound | xterm | ❌ | |

### Modes and cursor

| Sequence | Name | From | mwt | Note |
|---|---|---|:-:|---|
| `CSI ? 25 h` | DECTCEM, cursor visible | VT220 | ✅ | |
| `CSI ? 47 h`, `CSI ? 1047 h` | alternate screen | xterm | ✅ | 1047 clears it on leaving, as xterm |
| `CSI ? 1049 h` | alternate screen with saved cursor | xterm | ✅ | |
| `CSI ? 1048 h` | save and restore the cursor | xterm | 🟡 | works, but DECRQM answers 0 |
| `CSI ? 66 h` | DECNKM | VT320 | 🟡 | as `ESC =` and `ESC >` |
| `CSI Ps SP q` | DECSCUSR, cursor style | VT520 | 🟡 | block, underline and bar; the blinking styles are drawn steady |
| `CSI ? 12 h` | blinking cursor | xterm | ❌ | DECRQM answers 4 |
| `CSI ? 2026 h` | synchronized output | contour | ❌ | planned (TODO) |
| `CSI ? 2027 h` | grapheme clusters | contour | ❌ | DECRQM answers 0; planned (TODO) |

### Mouse, focus and paste

| Sequence | Name | From | mwt | Note |
|---|---|---|:-:|---|
| `CSI ? 1000 h` | button events | xterm | ✅ | |
| `CSI ? 1002 h` | button and drag events | xterm | ✅ | |
| `CSI ? 1003 h` | all motion | xterm | ✅ | |
| `CSI ? 1006 h` | SGR mouse encoding | xterm | ✅ | |
| `CSI ? 9 h`, `1001`, `1005`, `1015` | X10, highlight tracking, UTF-8 and urxvt encodings | xterm, urxvt | ❌ | DECRQM answers 4 |
| `CSI ? 1007 h` | alternate scroll | xterm | 🟡 | always on; `CSI ? 1007 l` is ignored |
| `CSI ? 1004 h` | focus events | xterm | ✅ | |
| `CSI ? 2004 h` | bracketed paste | xterm | ✅ | |

### Reports

| Sequence | Name | From | mwt | Note |
|---|---|---|:-:|---|
| `CSI ? 6 n` | DECXCPR | VT420 | ✅ | |
| `CSI > c` | DA2 | VT220 | ✅ | `CSI > 1 ; 10 ; 0 c`; only for 0 or none, as xterm |
| `CSI > q` | XTVERSION | xterm | ✅ | `DCS > \| mwt(version) ST`; only for 0 or none, as xterm |
| `CSI ? Ps $ p`, `CSI Ps $ p` | DECRQM | VT320 | ✅ | DEC and ANSI modes; 4 for modes mwt knows of but does not have |
| `CSI 18 t` | XTWINOPS, size in characters | xterm | 🟡 | only 18; the other window operations are ignored |
| `CSI Pi ; Pg ; Pt ; Pl ; Pb ; Pr * y` | DECRQCRA, checksum | VT420 | 🟡 | only with `allow_checksum` in config.h, off by default as in xterm |
| `DCS $ q … ST`, `DCS + q … ST` | DECRQSS, XTGETTCAP | VT420, xterm | ❌ | DCS queries are left out |

### SGR

| Parameter | Name | mwt | Note |
|---|---|:-:|---|
| 2, 3, 8, 9 | dim, italic, invisible, strike | ✅ | |
| 6 | rapid blink | 🟡 | as 5: stored, drawn steady |
| 21 | double underline | 🟡 | drawn as a single underline |
| `4:0` to `4:5` | underline styles | 🟡 | only on or off |
| 22 to 29 | reset bold and dim, italic, underline, blink, reverse, invisible, strike | ✅ | |
| 30–37, 40–47, 39, 49 | 8 colors, default colors | ✅ | |
| 90–97, 100–107 | bright colors | ✅ | |
| `38;5;n`, `48;5;n` | 256 colors | ✅ | also with colons |
| `38;2;r;g;b`, `48;2;r;g;b` | truecolor | ✅ | also with colons, with or without the color space |
| `58:…` | underline color | 🟡 | parsed, not drawn |

### OSC

| Sequence | Name | mwt | Note |
|---|---|:-:|---|
| `OSC 0`, `OSC 2` | window title | ✅ | ended by BEL or ST |
| `OSC 1` | icon title | ❌ | |
| `OSC 4`, `OSC 10`, `OSC 11`, `OSC 12` | palette, foreground, background, cursor color | 🟡 | queries are answered with the configured colors; setting is ignored |
| `OSC 104`, `OSC 110` to `119` | reset colors | ❌ | |
| `OSC 7`, `OSC 8`, `OSC 52` | working directory, hyperlinks, clipboard | ❌ | |

### Encoding

| Feature | mwt | Note |
|---|:-:|---|
| UTF-8 | ✅ | the only encoding |
| Wide characters | ✅ | widths after `wcwidth()`, as glibc and bash count them |
| Combining characters, ZWJ, variation selectors | ❌ | dropped (CAVEATS.md, Character widths) |
| 8-bit C1 controls (0x80–0x9F, U+0080–U+009F) | ❌ | dropped; only the 7-bit forms such as `ESC [` work |
| Characters that are not ASCII within a sequence | 🟡 | end the sequence; xterm puts a combining mark on the character before, ignores a C1, and reads U+00A0 to U+00FF as ASCII |
| DCS, SOS, PM, APC | 🟡 | parsed and swallowed up to ST, CAN or SUB, without effect |


## Also left out

Tektronix, Sixel and ReGIS; rectangle operations (DECCRA, DECFRA, DECERA); inserting and
deleting columns (DECIC, DECDC, DECBI, DECFI); protected areas and selective erase
(DECSCA, SPA, EPA, DECSED, DECSEL); national character sets, G2 and G3, SS2 and SS3;
XTSAVE and XTRESTORE; DECSCL; and the DECDSR reports for printer, keyboard and locator.
`make esctest` lists them as failures.
