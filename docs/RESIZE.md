# Resize

What `vt_resize()` does to the main screen and the history when the window gets
another size: the rules, and what was measured. Rules have identifiers (`J3`,
`E1`) that code comments and tests name. "Step N" is the commit "Reflow,
Schritt N".


## 1. Model

### 1.1 Terms

- **Blank**: a cell with a space, the default foreground and no attribute.
  It has a background.
- **Written cell**: one a print operation put there (`ATTR_TEXT`). Erasing
  makes it a blank that is not written.
- **Tail** of a row: a background colour. It stands for all the cells right of
  the row's content, as far as any width goes. A **tail blank** is a blank,
  not written, in the row's tail colour.
- **Join** of a row: 0, or the number of its cells after which the line goes
  on in the next row.
- **Gap cell**: a cell at or behind the join of a row that has one. Not part
  of the line.
- **Content** of a row: its cells `[0, e)`. With a join, `e` is the join.
  Without one, `e` is behind the last cell that is written or is not a tail
  blank; 0 if there is none.
- **Line**: a longest run of rows in which each but the last has a join, over
  the history and the main screen together. Its content is the content of its
  rows in order, its tail the tail of its last row. It **ends empty** if it has
  more than one row and the last has no content.
- **Document**: the lines of the history and the main screen, in order.
- **Line position**: an index into a line's content, or behind its end by a
  number of cells.

### 1.2 State of the main screen

| | State | Set by |
|---|---|---|
| S1 | per row: the cells | 2.1 to 2.3 |
| S2 | per row: the join | J rules |
| S3 | per row: the tail | E rules; always there, not only for a full row |
| S4 | per cell: written or not | W1, E rules |
| S5 | the cursor: column, row, wrap pending | |
| S6 | the saved cursor, the same three, or none: never set since the last reset | `DECSC`, `SCOSC`, 1048, 1049; `RIS` and `DECSTR` make it none |
| S7 | behind the alternate screen: the place the cursor left on the main one | X2 |
| S8 | the history: rows at the current width, each with its join and tail | H rules |

The alternate screen has cells, joins, tails, a cursor and a saved cursor by
the same rules W, E and J. It has no history and is not laid out (X1).

### 1.3 Invariants

They hold after every operation and after every resize.

- **I1.** `0 <= join <= cols` for every row.
- **I2.** The gap cells of a row with a join are tail blanks.
- **I3.** The last row of the screen has no join.
- **I4.** A cell that is not written is a blank.
- **I5.** The two halves of a wide glyph are in one row, next to each other,
  both in the content or both not.
- **I6.** The cursor is on the screen.
- **I7.** Nothing is kept that the screen and the history do not show, but
  the places of 2.9: where the cursor, the saved cursor and the screen's
  start were before a resize clamped them. No cell is kept.
- **I8.** On the main screen, and from the newest line of the history into
  its first row, every join is one the layout would make at this width: the
  row after it has no content (the line ends empty), or the join is `cols`, or
  it is `cols - 1` and the row after it begins with a wide glyph. So the rows
  of a line are what 2.4 gives for its content at the current width, and a
  row that scrolls into the history takes as many rows there as it took on
  the screen.


## 2. Rules

### 2.1 Writing

- **W1.** A printed character makes its cell, or its two cells, written, with
  the cursor's colours and attributes. So does each repetition of `REP`.
- **W2.** Printing does not change the tail of the row, also not in the last
  column: a wider terminal would show the tail's colour right of it.
- **W3.** Without autowrap, a character that does not fit is printed over the
  end of the row. No join is set.
- **W4.** A character of width 0 is dropped, one wider than the screen is too.

### 2.2 Erasing and the tail

An erase of the cells `[a, b)` of a row with background `B` makes them blanks
in `B` that are not written.

- **E1.** If the erase includes the last column (`b == cols`), the row's tail
  becomes `B`. That is so for `EL 0`, `EL 2`, `ED 0` and `ED 2` in every row
  they erase to its end, for `ED 1` in the rows above the cursor, for `ECH`,
  `EL 1` and the blanks `ICH` puts in where they reach the last column, for
  the blanks `DCH` brings in at the end of the row, and for every row made
  new: by scrolling, `IL`, `DL`, `SU`, `SD`, `RI`, `RIS`.
- **E2.** An erase that does not include the last column leaves the tail as it
  is. Its blanks are cells like any other: content up to the last cell of the
  row that is written or differs from a tail blank (1.1), nothing behind that.
- **E3.** `ICH` and insert mode shift cells to the right; those pushed past
  the last column are lost. The tail stays, but for E1. A wide glyph the
  shift cuts at the last column is erased.
- **E4.** `DCH` removes cells and shifts the rest left; E1 applies to the
  blanks at the end.
- **E5.** Half a wide glyph that is written over or erased takes the other
  half with it: that cell is erased in the cursor's background. The tail
  stays, also where that cell is in the last column: it is one cell mended,
  not the row's end erased. Where the erase itself reaches the last column,
  E1 applies to it as to any.

The cells of a row's old tail that an erase in another colour leaves standing
before it (`DCH` in blue in a row erased in red) are no longer tail blanks.
They are content from then on.

### 2.3 Joins

- **J1.** With autowrap, a character printed while a wrap is pending sets the
  join of the cursor's row to `cols` and goes to the next row. Not where the
  cursor stays in its row: in the last row of the screen below a scroll
  region, the character goes to the first column of the same row, and no join
  is set. The same holds for J2.
- **J2.** With autowrap, a wide glyph that does not fit in the last cell goes
  to the next row. The join of the row it leaves is `cols - 1` if the last
  cell is not content, by 1.1 for a row without a join, and `cols` if it is.
- **J3.** The join of a row is cleared when
  - (a) the last cell before it is written or erased, other than by a shift
    (J4),
  - (b) an operation leaves a gap cell that is not a tail blank: I2 is kept by
    ending the line,
  - (c) `EL`, `ED` or `ECH` erases the row's last column,
  - (d) `DCH` removes cells before it.
- **J4.** `ICH` and insert mode before the join move it right by the cells
  put in, to `cols` at most. J3a then applies to the cells put in, with the
  join where it is now; not to a wide glyph the shift cut (E3).
- **J5.** When rows move (`IL`, `DL`, `SU`, `SD`, `RI`, and `LF`, `IND`, `NEL`
  at the bottom of a scroll region), a row keeps its join only if the row that
  follows it afterwards is the row that followed it before. The newest line of
  the history counts as the row before the first row of the screen, and a row
  that scrolls into the history is still followed by the one that was below
  it. One exception: the row J1 or J2 has just set a join on keeps it, the
  line goes on in the new row.
- **J6.** Erasing a row that a line went on into does not clear the join of
  the row before it. The line then ends empty. (A line editor erases the row
  and writes it again.)
- **J7.** When the screen is erased from its first cell (`ED 0` with the
  cursor at the home position, `ED 1`, `ED 2`, `RIS`), the newest line of the
  history ends: it does not go on in the first row. `EL` in the first row does
  not do that, by J6.
- **J8.** A resize sets joins only by L2 and L3. It does so without autowrap
  too (D3).
- **J9.** On the main screen, when an operation gives a row its first
  content, or changes what it begins with, and the join of the row before is
  then not one I8 allows, that join is cleared. For the first row of the
  screen the row before is the last row of the history's newest line, as in
  J5. A join set on a row that had no content (J1, J2) gives it some: the
  cells before a join are content, blanks too. A row that began with the
  wide glyph a line wrapped for and now begins with something else is such a
  case, and so is a row a resize left empty behind a short join (L3) once
  something is written into it. A join of `cols` stays: writing again into
  the row a full row went on into keeps the line (J6).

### 2.4 Layout

A line is laid out in rows of `W` cells when the width changes:

- **L1.** Its content cells are placed left to right. A wide glyph takes two
  cells of one row.
- **L2.** When the next cell, or the next wide glyph, does not fit, the row is
  closed with a join of the number of cells placed, `W` or `W - 1`. A gap cell
  is a tail blank.
- **L3.** The row in which the content ends is closed without a join. If the
  line ends empty, that row is closed with a join of the cells placed in it,
  and one row without content follows.
- **L4.** A line without content, or with none that is placed (L6), is one
  row.
- **L5.** The tail of every row of the line is the line's tail. The cells of
  the last row behind the content are tail blanks.
- **L6.** At `W = 1`, wide glyphs are not placed, and are gone (K6).

Nothing else about a line changes: no cell gets another character, colour or
attribute, none is added to the content, and none leaves it but by L6. With a
wide glyph that ended the line, L6 takes the tail blanks before it out of the
content as well: they are the line's end then, and no content by 1.1. Where
they began a row, the line ends empty.

### 2.5 The cursor

The cursor, the saved cursor (S6) and the place behind the alternate screen
(S7) are mapped the same way when the width changes, from the places K1 kept
where they hold (K2).

- **C1.** *To a line position.* A pending wrap counts as the column behind the
  last one, `cols`, wherever the cursor is. In the last row of its line, at
  column `x`: the cells of the line's rows before it, plus `x`. In a row with
  a join `j`: the cells before it, plus the smaller of `j` and `x`. A cursor
  on a gap cell, or pending in a row that goes on, is at the first cell of
  the next row.
- **C2.** *From a line position inside the content:* the cell that holds that
  content cell, no wrap pending. If that cell is not placed (L6): the next one
  that is, and C3 if there is none.
- **C3.** *From a line position at or behind the end,* by `d` cells: let `f`
  be the content cells placed in the line's last row; 0 if the line ends
  empty.
  - `f + d < W`: column `f + d` of that row.
  - otherwise, if `f == W` and autowrap is on: the last column, a wrap
    pending. The next character goes to a new row, as after the character
    that filled this one.
  - otherwise: the last column, no wrap pending.
- **C4.** No row is made for a cursor behind its line's content (D4).
- **C5.** The cursor's row is on the screen (2.6). A saved cursor whose row is
  not is moved to the nearest row of the screen, in its column, no wrap
  pending. No saved cursor (S6) stays none: `DECRC` then goes home, as on a
  terminal of the new size that was given the same bytes.

### 2.6 Which rows the screen shows

The rows of the document at the new size are numbered from 0: laid out anew
for another width, as they are for another height alone (K5). The screen shows
`R1` of them, from row `t` on; the rows before `t` are the history.

Read from the screen before the resize, or from the places K1 kept where they
hold (K2): its height `R0`; `e0`, the last row of its screen with content, or
with a tail that is not the default, or the cursor's row if that is lower;
and whether `e0` is its last row (the screen was *full*). While the alternate
screen is shown, S7 stands for the cursor.

`S` is the row that holds the first cell of that screen's first row, `E` the
row that holds the place behind the last cell of row `e0`, and `c` the
cursor's row; each by C1 to C3 at another width. `c <= E` always.

In this order:

- **V1.** `t = S`: the screen starts where it started.
- **V2.** If the screen was full: `t = min(t, E - (R0 - 1))`. The end stays in
  the last row, and rows come back from the history when the content needs
  fewer.
- **V3.** `t = t - max(0, R1 - R0)`: a taller window takes that many more
  rows from the history.
- **V4.** `t = max(t, E - (R1 - 1), 0)`: the end is on the screen.
- **V5.** `t = min(t, c)`: the cursor's row is on the screen, and that wins
  over the end.
- **V6.** Rows below the screen are lost. Rows the document does not have are
  new: no content, the default tail.

### 2.7 The history

- **H1.** A row that leaves the first row of the screen upwards is added to
  the history: to its newest line if that goes on, as a new line otherwise.
  That is `LF`, `IND` and `NEL` at the bottom of a region that starts in the
  first row, `SU` there, and `DL` in the first row. Not on the alternate
  screen. Out of memory, the row goes in without content. By I8 the row counts
  in the history as it did on the screen. What the history does not keep of a
  row before the last of its line is that row's own tail: its gap cell is
  shown in the line's tail from then on.
- **H2.** A resize changes no line of the history but by H3. Only its layout
  is another.
- **H3.** The limit counts rows at the current width. Rows beyond it are
  removed: the oldest line loses its first rows, and lacks its beginning from
  then on. So a line is never longer than the limit allows.
- **H4.** `ED 3` empties the history, from either screen. The first row of the
  screen then begins a line.
- **H5.** Rows have numbers that count on from the oldest row of the history
  (`vt_abs_first()`). A resize keeps the number of the oldest row; the others
  follow from the rows at the new width. H1 does not give a row that stays
  another number.
- **H6.** A taller window, or a layout with fewer rows, shows rows of the
  history on the screen again (V2, V3).

### 2.8 The alternate screen

- **X1.** A resize does not lay it out. Its rows are cut on the right or
  padded there with default blanks, which is an erase of the new cells (E1,
  J3b); a wide glyph cut in half is erased, a join behind the cut is cleared.
  Rows are taken from the top or the bottom so that the cursor stays, and
  the join of the row that is then the last is cleared. I8 and J9 are not for
  this screen: a wider window leaves its joins where they were. Nothing of it
  is kept for another size (D6).
- **X2.** While it is shown, S7 holds the main screen's place: the cursor as
  it was when the alternate screen was entered. It is used for V5 and comes
  back as the cursor only by `DECRC` (mode 1049); modes 47 and 1047 leave the
  cursor where the alternate screen had it, clamped. It is a place in the line
  and no cursor: behind a row its line's content fills it stays behind it
  without autowrap too, and the next width reads it there.

### 2.9 The places kept

A resize clamps places. The cursor behind the end of its line is put into the
last column of a narrower window (C3), the saved cursor into a row of the
screen, and the screen's start is moved by V2 to V5. The state shows them
clamped, and from there another size cannot tell where they were. So a resize
keeps them as it found them, and the next one starts from there. No cell is
kept.

- **K1.** A resize keeps, next to the state it gives: the column of the
  cursor (of S7 while the alternate screen is shown) and of the saved cursor
  before the last column stopped them, counted from their row's first cell,
  past the last one as far as it goes; the saved cursor's row, counted from
  the first row of the screen, above and below the screen too; the row and
  the cell the screen started with before the resizes since, counted from
  its first row now; that screen's height, and whether its text reached its
  last row (2.6: it was full); and the row the text ends in after the resize
  (2.6: `e0`).
- **K2.** A resize reads C1 to C3 from the columns and the row K1 kept, and
  V1 to V5 from the start, the height and "full" K1 kept, where they hold
  (K3, K4); from the state otherwise. A saved cursor whose row went beyond
  the history's limit or below the screen has no row left: the nearest row,
  as the state shows it, stands for it. A start beyond the history's far end
  is the oldest row there is. For a screen that was full the start has no
  say: V2 and V4 put the end of the text in the last row, whatever row the
  screen started in.
- **K3.** The cursors' places hold until the program writes to the main
  screen: any byte written while it is shown, and leaving the alternate
  screen. The program has seen the size and goes on from where the cursor is
  now. Not what is written while the alternate screen is shown: a
  full-screen program draws after every change of size, and none of it is on
  the main screen.
- **K4.** The screen's start holds until rows of the main screen scroll
  (2.10: every operation that moves rows), until `vt_reset()`, and as long
  as the text ends in the row the last resize left it in. A write that does
  none of these leaves it: a shell that writes its prompt again after a
  change of size keeps the screen it had. Cleared, or written further down,
  it is another screen, and what was kept of the one before ("it was full")
  is not said of it.
- **K5.** A change of the height alone keeps the places and reads them, as a
  change of the width does: V1 to V5 in rows at the same width (D12).
- **K6.** What the places do not bring back is what a size on the way cut:
  rows beyond the history's limit, rows below the screen, wide glyphs at one
  column. Nor a wrap that was pending where the row's text does not reach the
  last column: C3 gives no such place.

With them A1 holds for a way on which nothing was cut and nothing written.
Where the start of the screen holds past a write (K4), A1 is not asked: the
state then says more than the screen and the history show (I7).

### 2.10 Every operation, and the rules it falls under

| Operation | Rules |
|---|---|
| print, `REP` | W1 to W4, E5, J1, J2, J3a, J3b, J9; in insert mode E3, E1, J4 |
| `CR` `BS` `HT` `CHT` `CBT`, cursor movement, `DECOM` | cursor only |
| `LF` `VT` `FF` `IND` `NEL` | cursor; at the bottom of the region E1, J5, H1 |
| `RI` | cursor; at the top of the region E1, J5 |
| `EL 0`, `EL 2` | E1, E5, J3c, J6, J9; drop a pending wrap |
| `EL 1`, `ECH` | E2, E5, J3a, J3b, J9; E1 and J3c at the last column; drop a pending wrap |
| `ED 0`, `ED 1`, `ED 2` | as `EL`, row by row, J9 with it; J7 |
| `ED 3` | H4 |
| `ICH` | E3, E1, E5, J4, J3a, J3b, J9; drops a pending wrap |
| `DCH` | E4, E1, E5, J3d, J3b, J9; drops a pending wrap |
| `IL` `DL` `SU` `SD` | E1, J5; `SU` and `DL` from the first row H1; `IL` and `DL` the cursor to column 0 |
| `DECSTBM` | the cursor home; a resize resets the region |
| `DECSC` `DECRC` `SCOSC` `SCORC`, 1048 | S6, cursor; without a saved one, `DECRC` goes home |
| 47, 1047, 1049 | X1, X2; leaving ends the cursors' places (K3) |
| `DECAWM` | W3, J1, J2 and C3 read it |
| `RIS` | all rows new (E1), J7, cursor home, no saved cursors, modes, tab stops, region; not the history |
| `DECSTR` | modes, region, the cursor's attributes; no saved cursor on the screen that is shown |
| `HTS` `TBC` | nothing here; a resize resets the tab stops |
| `vt_scroll_view()` | nothing here; a resize resets the view |
| `vt_resize()` | L, C, V, H2, H3, H5, H6, X1, K1 to K6 |


## 3. What the tests check

**What "the same state" compares** (`same_state()` in `test_vt.c`): every row
of the main screen with its cells (written or not included), join and tail;
every row of the history as the row interface gives it, with its tail; the
cursor, the saved cursor (or that there is none) and S7, each with column,
row and pending wrap; the number of rows in the history and the number of the
first one (H5). Not compared: the alternate screen and its cursors (X1), the
tab stops, the scroll region and the view (a resize resets them), and what is
to be redrawn.

- **A0. Invariants.** I2, I3 and I8 after every operation and every resize,
  for the kept fuzz findings, random streams with resizes in between, and the
  recordings of `make replay`.
- **A1. No trace of the way.** For any state and any sizes `s1 .. sn` with
  nothing written in between: resizing through `s1 .. sn` gives the same state
  as resizing to `sn` alone; with `sn` the size at the start, the state at the
  start. For plain output, through any sizes of two columns or more with a
  history that holds every row, as a test; for everything else as a count
  (M3), which K6 keeps above none.
- **A2. Plain output.** For a stream of printable characters of width 1 and 2,
  `SGR` without a background colour, and `CR LF` pairs, written to a terminal
  just reset, with autowrap on: writing it at `(W0, R0)` and resizing to
  `(W1, R1)` gives the same state as writing it at `(W1, R1)`. Provided the
  history reached its limit in neither, and `W0 >= 2` if the stream has wide
  characters (W4 drops them at one column).
- **A4. The same size.** A resize to the current size does nothing.
- **A5. The cursor.** After a resize it is on the screen, at the line position
  it had, or where C3 puts it. After a round trip it is where it was (A1).
- **A6. Out of memory.** With each allocation of a resize failed once: the
  same state as before.
- **A7. Every rule has a test.** Each rule is named in a comment at the code
  that implements it and in a check in `test_vt.c` that fails with the rule
  broken.
- **A10. The recordings.** `replay.sums` changes only where a rule says so,
  and each change says which sums and by which rule; none is regenerated
  wholesale (M5).


## 4. Decisions

| | Question | Decided |
|---|---|---|
| D1 | Is a written blank at the end of a line content? | Yes, every written cell is: a terminal of another width shows it given the same bytes. foot, kitty and Ghostty do the same. Costs a row only where a line of 79 characters ends in a blank (M2). |
| D2 | Does an `ECH`, `ICH` or `DCH` that reaches the last column set the tail? | Yes, one rule for every erase (E1). |
| D3 | Does a resize join rows while autowrap is off? | Yes: content is kept. |
| D4 | A cursor moved behind the end of its line: lay out blanks up to it? | No, C3 clamps, and the places bring the column back (K2). |
| D6 | The alternate screen: kept for another size? | No: full-screen programs redraw. |
| D7 | Does erasing a row end the join into it? | Open; J6 says no. |
| D8 | Which tail has a line of several rows? | Its last row's, the one that shows. |
| D9 | Does writing the last cell before a join end the line? | Yes (J3a). |
| D10 | Top or bottom? | V1 to V5. |
| D12 | A change of the height alone: by 2.6 too? | Yes. A taller window after Ctrl-L shows the rows from before it above the cleared screen again, until it is as tall as it was; as on `main`. |
| D13 | Rows that move: clear the joins between rows that are no longer neighbours? | Yes (J5). |
| D14 | The history's limit in rows of the current width? | Yes (H3). |
| D15 | A wide glyph at one column: left out, or a placeholder? | Left out (L6). |
| D16 | A saved cursor whose row is off the screen: moved onto it? | Yes (C5). |
| D17 | While a window is dragged: tell the program each size, or only the last? | Open; each size. |
| D18 | Prompts marked by the shell (OSC 133): treat them apart? | No. |
| D19 | Keep the content a resize cut (an origin), and the history as lines? | Not built. The places of 2.9 instead: a tenth of the code, an eighth of the cases left (M3). |
| D20 | A saved cursor that was never set: recorded as none? | Yes (S6, C5). |
| D21 | `ED 2` on the main screen: keep what it erases in the history? | No, as xterm, foot, Konsole and kitty: a program that redraws the main screen with `ED 2` would fill the history with its pictures. |


## 5. Results

Measured on the VT core, without a pty, unless it says otherwise.

- **M1. Plain output.** Random plain streams written at one size and resized
  to another, against the same stream written at the other size (widths 2 to
  41, heights 1 to 12, wide characters, attributes, foreground colours): 0 of
  40000 differ in rows, joins, cursor and history. Before step 6a the saved
  cursor differed in 18898 of 40000 (D20); before step 5, lines that end in
  blanks in 205 of 20000 (D1).
- **M2. Written blanks at a line's end** (D1), recorded with `script` at 80x24
  on a Debian host, 4379 lines of `ls`, `git`, `systemctl`, `ps`, `df`, `ip`,
  `top`, `watch` and an interactive bash: 97 end in one written blank, 132 in
  two to seven, none in more. That is 4 rows more of 5133 at 79 columns, and
  none at 60 or 40. `ps` and `top -b` write lines of 79 characters and a
  blank, the only cost found. readline (bash 5.3), zle (zsh 5.9.2) and fish
  4.9.3 cost no row.
- **M3. Round trips** (A1). Random streams with resizes in between, 100000
  cases a seed, seeds 1, 7 and 3; a case counts once, by the first thing that
  differs after a round trip or between one step and two to the same size.
  Trips that lose text are not counted.
  - Before the places (step 6b): 4815 cases, 9621 with 5000 rows of history.
    Most in the cursor and the saved cursor behind the end of their line, and
    in where the screen starts.
  - With the places (step 8): 633, and 875 with 5000 rows. Left: a cursor
    waiting to wrap (326, mostly the saved one), the cell a wide glyph did not
    fit in (27), no autowrap (5), the history at its limit (103), right after
    a change of the height (84) or of the width (27), other (62).
  - Plain output: none of 26450 cases a seed.
  - The checks of one resize from the kept places: none of 100000 cases
    fails, on seeds 1, 7, 3 and 11.
- **M4. Shells.** After a change of size bash 5.2.21 and 5.3.20 write `CR`,
  `EL`, `CR` and the last line of their prompt, zsh 5.9 and 5.9.2 `ED 0` and
  the prompt, fish 3.7.0 and 4.9.3 nothing. After Ctrl-L and output that does
  not fill the screen, 80 to 40 columns and back gives the screen as it was
  with each of the three, for prompts of one and two lines and for a screen
  cleared at 120 columns. With a command line typed and not entered that takes
  two rows at 40 columns, bash and zsh scroll the screen when they write it
  again, and the rows from before the Ctrl-L come back (K4, CAVEATS.md).
- **M5. The recordings** (A10). Step 4, by J5: `torturet.vt`, `xmas-05.vt`,
  `xmas-02.vt`, `xmas.large` and `xmas2.vt`; the cells are the same, joins are
  gone, none is new. Step 5, by D1 and H1: 73 recordings, only rows of the
  history longer by the blanks written at their end. The other steps changed
  none.
- **M6. The build server** (i5-1135G7, gcc 16.2.1), with step 8: `make check`,
  `make terminfo`, `make replay`, `make evil`, `make fuzz` (600 s, about
  239000 runs), `make fonts` and `make oldwayland` pass.
- **M7. Speed.** The VT core on 64 MB of text, run in turn, the median of 5 to
  15 runs. The machine's speed changes between sessions by more than the
  differences; only numbers of one run compare. Where the code lies counts as
  well: the same source with its code moved by 16 to 48 bytes is 1 to 2 %
  faster or slower, CJK text on the branch up to 10 %. So each build is
  measured at four such offsets and their mean compared.
  - ASCII text was 3 to 4 % slower than on `main` since the commit before
    step 1, which gave a row a third field before its cells. With 12 bytes
    before them, every fourth cell's 8-byte store in `fill_row()` and `put()`
    goes across a cache line; with 8, 16 or 20 none does, and with 28 it is
    slow again. `put_ascii()` stores 4 bytes at most and lost nothing: ASCII
    without line feeds is as fast as on `main`. The cells start 16 bytes in
    since, and ASCII text is as fast as on `main`.
  - CJK text is about 8 % slower than on `main`: 2 to 3 % of it the same
    stores, 4 to 5 % `put()` asking `joins_settled()` after every character
    and calling `row_changed()` where that says so (step 3). Without check
    and call, which is no terminal, CJK is 2 % behind `main`. Not the work of
    the check: asking once for a row wins 1 %, `row_changed()` out of line
    and cold nothing; what costs is the call in `put()`, which grows from 238
    to 293 instructions with it. The layout of `struct vt` and `ATTR_TEXT` in
    the cells change nothing.
  - In instructions (cachegrind, 6 MB) the branch adds less than it loses in
    time: CJK 750.7 million for 726.0 on `main` (3.4 %), ASCII 0.5 and 1.4 %.
