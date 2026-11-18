/* util.h - small pieces of mwt.c without Wayland: the -g argument, the
 * context menu's keys. test_util checks them. */
#ifndef UTIL_H
#define UTIL_H

enum { GEOM_OK, GEOM_SYNTAX, GEOM_RANGE };

/* COLSxROWS, each from 1 to its max, into cols and rows on GEOM_OK. As strtol
 * reads them: a sign and leading blanks are taken. */
int parse_geometry(const char *s, int max_cols, int max_rows, int *cols, int *rows);

/* The menu item Up (dir < 0) or Down (dir > 0) goes to from hover, -1 for none,
 * among n items, bit i of enabled set for each that can be chosen: the next one
 * that can, round the end. Down from none is the first, Up the last. hover when
 * none can be chosen. */
int menu_next(int hover, int n, int dir, unsigned enabled);

#endif
