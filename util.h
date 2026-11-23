/* util.h - small pieces of mwt.c without Wayland: the -g argument, the
 * context menu's keys, the standard fds. test_util checks them. */
#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { GEOM_OK, GEOM_SYNTAX, GEOM_RANGE };

/* COLSxROWS, each from 1 to its max, into cols and rows on GEOM_OK. As strtol
 * reads them: a sign and leading blanks are taken. */
int parse_geometry(const char *s, int max_cols, int max_rows, int *cols, int *rows);

/* The menu item Up (dir < 0) or Down (dir > 0) goes to from hover, -1 for none,
 * among n items, bit i of enabled set for each that can be chosen: the next one
 * that can, round the end. Down from none is the first, Up the last. hover when
 * none can be chosen. */
int menu_next(int hover, int n, int dir, unsigned enabled);

#include <stddef.h>

/* The window's logical size for a grid of cols x rows, cells of cw x ch and pad
 * on each side in buffer pixels of the scale: rounded up, so that the buffer,
 * w * scale by h * scale, holds the grid. */
void snap_size(int cols, int rows, int cw, int ch, int pad, int scale, int *w, int *h);

/* cols and rows made to fit configure_bounds of bw x bh logical pixels (0: no
 * bound), with cells of cw x ch and pad on each side in buffer pixels of the
 * scale; one cell at least. KWin and mutter send bounds, sway does not. */
void fit_bounds(int *cols, int *rows, int bw, int bh, int scale, int pad, int cw, int ch);

/* How much of s fits in max bytes without cutting a UTF-8 sequence: -T and -a
 * go out in one Wayland message, whose size field is 16 bits. */
size_t utf8_clip(const char *s, size_t max);

/* Each of fds 0 to 2 that is closed opened on /dev/null, so that nothing mwt
 * opens later takes its place: a message for stderr would go into the pty or
 * the Wayland socket. False when /dev/null cannot be opened. */
bool stdio_fill(void);

/* The xdg-activation token mwt was started with, from XDG_ACTIVATION_TOKEN or
 * else DESKTOP_STARTUP_ID, and both names taken out of the environment: the
 * token is for mwt's window, and the shell would hand it on, stale, to what it
 * starts. An empty one, or one over TOKEN_MAX bytes, counts as none: no token
 * is that long. NULL for none or no memory. */
#define TOKEN_MAX 1024
char *startup_token(void);

/* A toast, as mwc shows one: a line of text in a box at the top of the window,
 * centered, for a while. Its sizes in logical pixels. */
#define TOAST_PAD 12    /* around the text */
#define TOAST_MARGIN 16 /* from the window's top and sides */
#define TOAST_MS 3000
#define TOAST_MAX 64    /* cells shown at most */
#define TOAST_TEXT 256  /* code points kept */

/* "name: not found" for ENOENT, else "name: " and strerror(err) as the C locale
 * says it, in buf: the toast is English whatever the locale, as mwt's own
 * words in it are. */
void toast_text(char *buf, size_t size, const char *name, int err);

/* s as code points, at most max into cp: what is not UTF-8 one U+FFFD a byte.
 * Their count. */
int toast_cps(const char *s, uint32_t *cp, int max);

/* n code points cut to fit from the left, so that the end, which says what went
 * wrong, stays: an ellipsis, then the last fit - 1. The count left. */
int toast_fit(uint32_t *cp, int n, int fit);

/* Where a toast of n cells goes in a window of win_w x win_h, with cells of cw x
 * ch buffer pixels at scale: n cut to what fits between the margins, the box in
 * logical pixels. False when not one cell fits. */
struct toast_box { int n, x, y, w, h; };
bool toast_place(int n, int win_w, int win_h, int cw, int ch, int scale,
		 struct toast_box *b);

#endif
