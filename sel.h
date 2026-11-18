/* sel.h - the selection over the VT core's lines, without Wayland: mwt.c feeds
 * it the pointer and draws it, test_sel checks it headless. Lines are absolute
 * (vt_abs_line), so a selection stays on its text while the screen scrolls. */
#ifndef SEL_H
#define SEL_H

#include <stdbool.h>
#include <stdint.h>

#include "vt.h"

/* by click count: one click, a double click, a triple click */
enum { SEL_CHAR = 1, SEL_WORD, SEL_LINE };

/* anchor (aa, ac) and moving end (ea, ec), as the pointer left them */
struct sel {
	bool active, dragging;
	int mode;
	int64_t aa, ea;
	int ac, ec;
};

/* the last press of the left button, to count clicks */
struct clicks {
	uint32_t time;
	int count, col;
	int64_t abs;
};

/* The selection as drawn and copied, inclusive, begin before end: a word
 * selection grown to whole words at both ends, a line selection to whole lines
 * (ec INT_MAX). */
void sel_range(const struct vt *vt, const struct sel *s, int64_t *ba, int *bc,
	       int64_t *ea, int *ec);

/* Whether the cell at line abs, column col, span columns wide, is in the range. */
bool sel_covers(int64_t ba, int bc, int64_t ea, int ec, int64_t abs, int col, int span);

/* The selected text as UTF-8: trailing blanks go, a line that wraps joins the
 * next. NULL when nothing is selected or out of memory; the caller frees it. */
char *sel_text(const struct vt *vt, const struct sel *s);

/* The rows shown of absolute lines a to b get drawn anew. */
void sel_mark_lines(struct vt *vt, int64_t a, int64_t b);

/* A press of the left button at line abs, column col, at time ms: a click on the
 * cell of the last one within double_ms counts on, up to three, then from one.
 * Starts a drag; a double or triple click selects at once. */
void sel_press(struct sel *s, struct clicks *c, uint32_t time, int64_t abs, int col,
	       uint32_t double_ms);

/* Shift and the left button: the end moves there, the anchor stays. */
void sel_extend(struct sel *s, int64_t abs, int col);

/* The pointer, dragging, is at abs, col: true when the selection changed, and
 * then the rows it changed are marked. */
bool sel_drag(struct vt *vt, struct sel *s, int64_t abs, int col);

/* Everything down to the last line with text, the scrollback and the screen,
 * on the alternate screen only that; false when there is no text. */
bool sel_all(const struct vt *vt, struct sel *s);

#endif
