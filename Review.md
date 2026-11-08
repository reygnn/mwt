# Open review points

Two sources, both at commit `abafc03`, to which the line numbers refer: a second review of
the whole code base, whose findings were each checked against the code and reproduced where
possible, and a test coverage measurement with gcov. The code review of `a375b52..v0.3.3`
before them is done, see the end.

**Reproduced** means seen by feeding the VT core or running mwt under a headless sway.
**Read** means confirmed from the code only, not run.

## Findings of the second review

Each is fixed or decided, see the end; what is left are facts.

### Maintainability

Facts, not bugs: `mwt.c` has 2523 lines around the global state `t`; selection and
clipboard (about 550 lines) and the context menu could be files of their own. `struct vt`
is public, and `mwt.c` reads `scrolled`, `hist_count`, `mode` and `dirty` directly. The
README and the code are in English, `BUGS`, `CAVEATS` and `TODO` in German. Part of the
tests needs the mwc repository next to this one. CI (`.github/workflows/check.yml`) runs
what needs no compositor on every push: mwt built with warnings as errors, `make check`,
`make terminfo` and `make replay`.

## Test coverage

Places no test reaches, so a bug there would go unnoticed. None of them is a known bug.

Since the measurement, tests reach some of them; gcov has not been run again:
`evil.sh buffer_fail` runs `buffer_init` out of memory, `make fonts` the fallback fonts,
and `tests/font_oom.c` the synthesized bold or italic (a failing `strdup` in `load_face`
falls back to it) and `font_open` giving up. They are marked below.

| File | Lines run | Branches taken | Measured with |
|---|---|---|---|
| `vt.c` | 97.7 % of 1072 | 94.9 % of 867 | `test_vt`, `make replay`, the fuzz corpus (1753 inputs) |
| `mwt.c` | 85.1 % of 1566 | 71.6 % of 1269 | `make test`, `pixel`, `keys`, `mouse`, `keyboard` under headless sway |
| `font.c` | 69.4 % of 216 | 50.0 % of 152 | the same |

For `vt.c` alone: `test_vt` reaches 94.1 %, the fuzz corpus 92.9 %, the replay 64.8 %.

`mwt.c` and `font.c` were measured without the rest of `make evil` (`evil.sh`,
`evil-mwc.sh`, `leaks.py`, `width.sh`, esctest, the other keyboard layouts), which reach
more of them, among it pasting, `vt_reply` and `ctrl_latin`. Over the whole suite they are
higher; by how much was not measured. The tests end mwt with SIGTERM, and mwt has no
handler for it, so the measuring build added one that writes the counts first; that build
is not in the repository.

### VT core (`vt.c`)

- The mouse modes 1000, 1002, 1003, 1004 and 1006: setting them in `set_private_mode`
  (`vt.c:830`, `vt.c:836`) and reporting them in `private_mode_state` (`vt.c:869` to
  `vt.c:873`). Only `tests/mouse.py` checks them, through mwt under sway; no test of the VT
  core does.
- `vt_abs_line` for a line outside the screen and the history (`vt.c:211`, `vt.c:217`).
- The early returns of `scroll_up` and `scroll_down` for an origin outside the scroll region
  or nothing to scroll (`vt.c:252`, `vt.c:255`, `vt.c:272`, `vt.c:275`), and of
  `clear_line` for an empty range (`vt.c:171`).
- `set_cell` raising the row's count for a colored blank (`vt.c:89`): a wide glyph broken up
  while the background is set.
- DECRQCRA counting DEL and 0xFF as 0 (`vt.c:895`).
- `vt_free(NULL)` (`vt.c:452`) and `vt_key` for a key that is not one (`vt.c:1463`).

### Wayland side (`mwt.c`)

- Never called: `popup_repositioned` (`mwt.c:1384`), `tl_close` (`mwt.c:2123`),
  `tl_bounds` (`mwt.c:2128`), `surf_enter` and `surf_leave` (`mwt.c:2140`; the HiDPI test
  goes through `preferred_buffer_scale`), `reg_remove` (`mwt.c:2178`), `ptr_axis_stop`
  (`mwt.c:2020`), `ps_cancelled` (`mwt.c:500`), `drop_paste` (`mwt.c:743`), `die`
  (`mwt.c:179`) and `usage` (`mwt.c:2346`).
- `vt_reply` (`mwt.c:265`) and `vt_color` (`mwt.c:845`): no test under sway asks mwt for
  DA, DSR or a color. esctest does, but was not part of this measurement.
- Pasting the primary selection of another client (`paste`, `mwt.c:715`): the tests paste
  what mwt itself selected.
- Partly run:

  | Function | Lines run | What is missing |
  |---|---|---|
  | `ctrl_latin` (`mwt.c:1543`) | 21 % | Ctrl on non-Latin layouts |
  | `ptr_axis` (`mwt.c:1998`) | 30 % | touchpad scrolling, as opposed to a wheel with notches |
  | `spawn` (`mwt.c:2214`) | 41 % | the error paths |
  | `wait_child` (`mwt.c:2292`) | 45 % | a child that does not end in time |
  | `buffer_init` (`mwt.c:1005`) | 69 % | running out of memory, since: `buffer_fail` |
  | `send_text` (`mwt.c:451`) | 69 % | the error paths |

### Fonts (`font.c`)

- Fallback fonts (`find_fallback`, `font.c:212`): the image of `make evil` has no font
  beyond DejaVu, so no CJK or emoji, and the search for a fallback never runs there.
  `make fonts` covers it in an image of its own (`tests/Containerfile.fonts`): CJK and
  Hangul from two fallback faces, bold, an emoji as an outline, a zoom, and many scripts.
- A synthesized bold or italic when the family has no such face (`font.c:139`). Since:
  `font_oom.c`, through a failing `strdup`, not through a family without such a face.
- The font matrix from fontconfig (`font.c:63`), a size given in pixels (`font.c:102`), and
  the error paths: no scalable font (`font.c:134`), a size out of range (`font.c:161`), a
  face that does not load (`font.c:51`).

### Left open on purpose

- The drag-and-drop handlers (`dd_*`, `ds_*`, `offer_actions`) are never called. Drag and
  drop is in the README under "Deliberately left out"; they only answer the protocol.

## Done

The eight findings of the code review of `a375b52..v0.3.3`:

- A new font size in a window that keeps its size (tiled, maximized, fullscreen) left
  pieces of the old grid beside the new one (`eef0ba9`). `tests/pixel.py` zooms a tiled
  window step by step.
- Skipping the copy of rows a frame draws anew relied on `draw_row` covering every pixel of
  its row, unchecked; and the byte-for-byte check of each frame against a full redraw was
  not in the repository (`8b554af`). A build with `-DMWT_CHECK_REDRAW` checks every frame,
  and `make evil` runs `make pixel` with it.
- `profbench` ran at a fixed grid instead of `PROFILE_GRID`, `draw_row` worked out each
  cell's colors twice, `blit` formed a pointer before the start of a row, `profile-run.sh`
  had an unused awk variable, and the profiler counted libwayland's roundtrip callbacks as
  frames (`da8f22a`).

`make evil` passed after these (`da8f22a`), and passes at `abafc03`.

The three findings of the second review that were to fix (`d24ad43`):

- Reproduced: overlong UTF-8 was accepted (`vt_write`). Only values above U+10FFFF and
  surrogates were refused, so `E0 80 9B` and `F0 80 80 9B` decoded to a real ESC:
  `E0 80 9B [31m X` wrote a red X, past any filter that drops the byte 0x1B. The decoder
  now keeps the least value a sequence may encode and turns anything below into U+FFFD.
  `test_vt` checks overlong ESC and A and the shortest valid forms.
- Reproduced: REP worked around the read budget. It repeated up to 65535 times per
  sequence, and `read_pty` looks at the clock only between reads: 1024 times
  `CSI 65535 b`, 8 KiB, took 0.47 s in one `vt_write`. Reads from the pty were at most
  10 KiB, 2.2 KiB on average, so a stream of them stalled mwt for up to about 0.6 s per
  read. REP now repeats at most a screenful; more only added the same lines to the history.
  xterm repeats them all.
- Read: the glyph cache was not checked for allocation failures. `font_open` set
  `cache_cap` before an unchecked `calloc`, and `font_glyph` went on adding entries when
  growing failed, until `slot()` found no free place and looped forever. Both are checked
  now; with a full table a new glyph draws nothing. Only out of memory, checked by reading,
  not run.

Two of the points to decide, fixed (`4c75d2d`, `d4fbc16`). Both tests pass in the
container, and both fail with the old `mwt.c`:

- Read: `drop_paste_for_signal` sent a closing `\e[201~` whenever one waited in `t.out`,
  also when the opening one had not gone out. mwt now follows the brackets in what
  `flush_out` writes and closes only one that went out open. Without NOFLSH the kernel
  flushes the input on ^C, the closing bracket sent before it included, so this only
  matters to programs with NOFLSH. `evil.sh paste_bracket_on_signal`.
- Read: `paste_text` dropped only ESC and NUL, so ^C, ^Z and ^D in pasted text raised
  signals in the kernel. Now only tab, CR and LF stay of the C0 controls, and DEL goes too.
  `evil.sh paste_controls`.

The limit of fallback faces, found by `make fonts`, which passes with the fix:

- Measured: after 16 fallback faces (`MAX_FALLBACK`), a script with a font on the system
  drew nothing; `make fonts` drew 19 of 26 scripts. Now the face used longest ago makes
  room for a new one. Glyphs already drawn stay, the glyph cache holds bitmaps and no
  faces; a glyph not drawn before loads its face again. `tests/fallback.py`
  `test_many_scripts` wants all 26, then the first 8 in bold, from faces loaded anew.

Found while checking that change:

- Read: `load_face` did not check the `strdup` of the face's path, nor did `font_open`
  where it makes a bold or italic from the regular face. Out of memory, `path` was NULL,
  and `find_fallback` passed it to `strcmp`. Both are checked now: `load_face` closes the
  face and fails, `font_open` gives up. `tests/font_oom.c`, run by `make fonts`, fails each
  `strdup` of `font_open` and of the fallback faces once in turn, under sanitizers:
  all 8 pass with the fix; with the old `font.c` UBSan stops at the `strcmp` in
  `find_fallback`.

A search for further NULL pointers and unchecked lines, by reading (fontconfig's
handling of NULL tried against 2.15):

- Read: `buffer_init` (`mwt.c`) freed the old buffer before it had the new one. When
  `memfd_create`, `ftruncate` or `mmap` failed after that, the buffer kept its old size
  over freed memory, and a window back at that size drew into it. The new buffer is now
  made whole first and the old one freed only then; `wl_shm_create_pool` and
  `wl_shm_pool_create_buffer` are checked on the way. `evil.sh buffer_fail`, with
  `tests/failmemfd.c` making `memfd_create` fail on demand, passes; with the old `mwt.c`
  ASan stops mwt in `fill_rect`, writing into the freed buffer.
- Read, tried: `font_open` and `find_fallback` did not check `FcPatternDuplicate`.
  `FcPatternDel`, `FcPatternAdd*` and `FcConfigSubstitute` crash on a NULL pattern.
  `tests/font_oom.c` now fails each `FcPatternDuplicate` once as well: 8 strdups and 8
  `FcPatternDuplicate`s pass; with the old `font.c` the first run crashes in fontconfig,
  called from `font_open`.

Left as they are: `xkb_context_new` and the other Wayland constructors are not checked,
both only out of memory; `kb_keymap` relies on the keymap ending in NUL, as the protocol
says it does. Nothing found in `vt.c`, the selection and paste paths, the Wayland event
handlers, the terminfo lookup or the C test tools.

The rest of the second review:

- Read, partly reproduced: each OSC 0 or 2 was a `set_title` request right away, tens of
  thousands in one round of the event loop in a flood. libwayland 1.24 took 100,000;
  before 1.23, which includes the 1.22 of Ubuntu 24.04, it ends the connection when its
  buffer is full. `vt_title` now keeps the title, and the event loop sends the last one
  once a round, and only once the compositor has read all mwt sent before.
  `evil.sh title_flood` sets 100,000 and wants the last one shown. Counted with a
  counter in `send_title`: of 100,000 titles 37,364 go out, against 100,000 before; with
  sway stopped for 3 s in a million titles, 272 went out meanwhile, against 476,919 from
  a round each without the wait. Against libwayland 1.22 (Ubuntu 24.04 in Podman, sway
  1.9): the old code and a title each round both die in `title_flood` with "Error sending
  request: Resource temporarily unavailable"; with the wait it passes, also with sway
  stopped.
- Read: the glyph cache grew without a limit with ever new characters. It is now emptied
  at 65536 glyphs or 64 MiB of bitmaps. A negative cache for characters no font has is not
  needed, at 0.18 ms a new one (`make fonts`). `tests/font_cache.c` builds `font.c` in with
  small limits and checks both, and that a glyph comes back as it was.
- Read: `mwt.info` declared `blink`, and `sgr` sent it, but mwt never draws it. Both are
  gone; `vt.c` still takes SGR 5. `tests/terminfo.py` wants `sgr` to set nothing for it.
- Maintainability: CI now runs what needs no compositor, see above.

Run: `title_flood` passes; under libwayland 1.24 it did with the old code too, under 1.22
only with the wait for the compositor, see above. `font_cache` passes and fails three ways with the emptying taken out.
`terminfo.py` passes, and with the old entry reports `blink` and `sgr`. The CI steps pass
on the host; the workflow itself runs on the first push.
