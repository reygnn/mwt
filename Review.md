# Open review points

From a test coverage measurement with gcov at commit `abafc03`, after the code review of
`a375b52..v0.3.3`. Line numbers are for `abafc03`. None of these is a known bug: they are
places no test reaches, so a bug there would go unnoticed.

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

## VT core (`vt.c`)

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

## Wayland side (`mwt.c`)

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
  | `buffer_init` (`mwt.c:1005`) | 69 % | running out of memory |
  | `send_text` (`mwt.c:451`) | 69 % | the error paths |

## Fonts (`font.c`)

- Fallback fonts (`find_fallback`, `font.c:212`): the test image has no font beyond
  DejaVu, so no CJK or emoji, and the search for a fallback never runs.
- A synthesized bold or italic when the family has no such face (`font.c:139`).
- The font matrix from fontconfig (`font.c:63`), a size given in pixels (`font.c:102`), and
  the error paths: no scalable font (`font.c:134`), a size out of range (`font.c:161`), a
  face that does not load (`font.c:51`).

## Left open on purpose

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
