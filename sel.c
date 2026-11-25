/* sel.c - see sel.h */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "paste.h"
#include "sel.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static bool is_word(uint32_t cp)
{
	return cp > 0x7f || (cp > ' ' && !strchr("\"'`()[]{}<>|;,", (int)cp));
}

/* 0 blank, 1 word, 2 any other character. The right half of a wide character
 * has no character of its own, it is of the left half's class. */
static int cell_class(const struct vt *vt, int64_t abs, int col)
{
	int len;
	const struct cell *l = vt_abs_line(vt, abs, &len);
	if (l && col > 0 && col < len && (l[col].attr & ATTR_WDUMMY))
		col--;
	/* a blank with a mark on it is a mark shown by itself, not a blank */
	if (!l || col >= len || ((l[col].cp == ' ' || l[col].cp == 0) && !l[col].ext))
		return 0;
	return is_word(l[col].cp) ? 1 : 2;
}

void sel_range(const struct vt *vt, const struct sel *s, int64_t *ba, int *bc,
	       int64_t *ea, int *ec)
{
	*ba = s->aa; *bc = s->ac;
	*ea = s->ea; *ec = s->ec;
	if (*ea < *ba || (*ea == *ba && *ec < *bc)) {
		int64_t ta = *ba; int tc = *bc;
		*ba = *ea; *bc = *ec;
		*ea = ta; *ec = tc;
	}
	if (s->mode == SEL_LINE) {
		*bc = 0;
		*ec = INT_MAX; /* to the end of the line, whatever its length */
	} else if (s->mode == SEL_WORD) {
		/* blanks and words grow, a lone other character stays alone */
		int k = cell_class(vt, *ba, *bc);
		if (k != 2)
			while (*bc > 0 && cell_class(vt, *ba, *bc - 1) == k)
				(*bc)--;
		k = cell_class(vt, *ea, *ec);
		if (k != 2)
			while (*ec < vt->cols - 1 && cell_class(vt, *ea, *ec + 1) == k)
				(*ec)++;
	}
}

bool sel_covers(int64_t ba, int bc, int64_t ea, int ec, int64_t abs, int col, int span)
{
	return abs >= ba && abs <= ea && !(abs == ba && col + span - 1 < bc) &&
	       !(abs == ea && col > ec);
}

char *sel_text(const struct vt *vt, const struct sel *s)
{
	if (!s->active)
		return NULL;
	int64_t ba, ea;
	int bc, ec;
	sel_range(vt, s, &ba, &bc, &ea, &ec);
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (int64_t a = ba; a <= ea; a++) {
		int n;
		const struct cell *l = vt_abs_line(vt, a, &n);
		if (!l)
			continue;
		int c0 = a == ba ? bc : 0;
		int c1 = a == ea ? MIN(ec, n - 1) : n - 1;
		/* a line that goes on in the next ends at its wrap, also with the
		 * blanks of a window grown wider after it */
		int w = vt_abs_wraps(vt, a);
		bool wrapped = w > 0 && c1 >= w - 1;
		if (wrapped)
			c1 = w - 1;
		/* begun on a right half: the character is highlighted, so copied */
		if (c0 > 0 && c0 < n && (l[c0].attr & ATTR_WDUMMY))
			c0--;
		size_t start = len;
		for (int c = c0; c <= c1; c++) {
			if (l[c].attr & ATTR_WDUMMY)
				continue;
			if (!put_utf8(&buf, &len, &cap, l[c].cp ? l[c].cp : ' '))
				goto nomem;
			/* and the marks on it, which are not drawn */
			const uint32_t *ext;
			for (int k = 0, n = vt_cell_ext(vt, &l[c], &ext); k < n; k++)
				if (!put_utf8(&buf, &len, &cap, ext[k]))
					goto nomem;
		}
		if (!wrapped) {
			while (len > start && buf[len - 1] == ' ')
				len--;
			if (a != ea && !put_utf8(&buf, &len, &cap, '\n'))
				goto nomem;
		}
	}
	if (put_utf8(&buf, &len, &cap, 0))
		return buf;
nomem:
	free(buf);
	return NULL;
}

void sel_mark_lines(struct vt *vt, int64_t a, int64_t b)
{
	int64_t top = (int64_t)vt->scrolled - vt->view;
	int64_t lo = MAX(MIN(a, b) - top, 0), hi = MIN(MAX(a, b) - top, vt->rows - 1);
	for (int64_t r = lo; r <= hi; r++)
		vt->dirty[r] = true;
}

void sel_mark(struct vt *vt, const struct sel *s)
{
	if (s->active) /* a word or a line grows within its lines */
		sel_mark_lines(vt, s->aa, s->ea);
}

void sel_press(struct sel *s, struct clicks *c, uint32_t time, int64_t abs, int col,
	       uint32_t double_ms)
{
	/* unsigned: the millisecond clock may wrap between two clicks */
	if (time - c->time < double_ms && abs == c->abs && col == c->col)
		c->count = c->count % 3 + 1;
	else
		c->count = 1;
	c->time = time;
	c->abs = abs;
	c->col = col;
	s->mode = c->count;
	s->aa = s->ea = abs;
	s->ac = s->ec = col;
	s->dragging = true;
	s->active = c->count > 1;
}

void sel_extend(struct sel *s, int64_t abs, int col)
{
	s->ea = abs;
	s->ec = col;
	s->dragging = true;
}

/* Only what changes: the rows from the old end to the new, and the anchor's,
 * whose word or line extends the other way once the end passes it. Marking all
 * redrew the screen at pointer rate. */
bool sel_drag(struct vt *vt, struct sel *s, int64_t abs, int col)
{
	if (abs == s->ea && col == s->ec && s->active)
		return false;
	if (s->active) {
		sel_mark_lines(vt, s->ea, abs);
		sel_mark_lines(vt, s->aa, s->aa);
	} else {
		sel_mark_lines(vt, s->aa, abs);
	}
	s->ea = abs;
	s->ec = col;
	s->active = true;
	return true;
}

bool sel_all(const struct vt *vt, struct sel *s)
{
	int64_t first = vt->mode & MODE_ALTSCREEN ? (int64_t)vt->scrolled : vt_abs_first(vt);
	for (int64_t a = (int64_t)vt->scrolled + vt->rows - 1; a >= first; a--) {
		int n;
		const struct cell *l = vt_abs_line(vt, a, &n);
		for (int c = 0; l && c < n; c++) {
			if ((l[c].cp == ' ' || l[c].cp == 0) && !l[c].ext)
				continue;
			s->mode = SEL_LINE;
			s->aa = first;
			s->ea = a;
			s->ac = s->ec = 0;
			s->dragging = false;
			s->active = true;
			return true;
		}
	}
	return false;
}
