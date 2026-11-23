/* util.h - small pieces of mwt.c without Wayland: the -g argument, the
 * context menu's keys, the standard fds. test_util checks them. */
#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>

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
 * starts. NULL for none, an empty one, one over TOKEN_MAX bytes (no token is
 * that long, and the Wayland message would not fit) or no memory. */
#define TOKEN_MAX 1024
char *startup_token(void);

#endif
