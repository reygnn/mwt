/* link.h - URLs in the text the VT core holds, found by their scheme, and the
 * opener's command line, without Wayland: mwt.c underlines and opens them,
 * test_link checks them headless. */
#ifndef LINK_H
#define LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vt.h"

/* Cells of a logical line looked at on either side of the pointer: a URL longer
 * than that is cut there. */
#define LINK_SPAN 4096

/* A URL's first and last cell, absolute lines (vt_abs_line) as struct sel's, so
 * that sel_covers() tells whether a cell is in it. */
struct link {
	int64_t ba, ea;
	int bc, ec;
};

/* The URL at line abs, column col: one of http://, https://, ftp://, file://
 * and mailto: (any case) with something after it, not in the middle of a word,
 * over lines that wrap. It ends at a blank, a control, a box drawing character
 * or one of <>"`{}|\^; punctuation at its end and a closing bracket it did not
 * open are not part of it. True when there is one, then in *l and, if url is
 * not NULL, as UTF-8 in *url, which the caller frees. False too when out of
 * memory. */
bool link_at(const struct vt *vt, int64_t abs, int col, struct link *l, char **url);

/* Whether two links are the same cells. */
bool link_same(const struct link *a, const struct link *b);

/* The opener's argv: spec split at blanks into words (no quotes, no shell),
 * then url; into buf, which holds a copy of spec, and argv, max entries with
 * the closing NULL. The count of words, url included, or 0 when spec has none,
 * does not fit in size bytes, or has too many. */
int link_argv(const char *spec, const char *url, char *buf, size_t size, char **argv,
	      int max);

#endif
