/* vt.c - a small VT parser after Paul Williams' DEC state machine
 * (https://vt100.net/emu/dec_ansi_parser), fed with decoded UTF-8.
 * Implements the subset of xterm's ctlseqs that current software uses. */
#define _XOPEN_SOURCE 700
#include "vt.h"
#include "version.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum {
	ST_GROUND, ST_ESC, ST_ESC_INTER, ST_CSI_ENTRY, ST_CSI_PARAM,
	ST_CSI_INTER, ST_CSI_IGNORE, ST_OSC, ST_STR_IGNORE,
};

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(v, lo, hi) MIN(MAX(v, lo), hi)

static const uint16_t dec_graphics[32] = { /* 0x5f .. 0x7e */
	0x00a0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0x00b0,
	0x00b1, 0x2424, 0x240b, 0x2518, 0x2510, 0x250c, 0x2514, 0x253c,
	0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534,
	0x252c, 0x2502, 0x2264, 0x2265, 0x03c0, 0x2260, 0x00a3, 0x00b7,
};

/* ---- helpers ---------------------------------------------------------- */

__attribute__((format(printf, 2, 3)))
static void reply(struct vt *vt, const char *fmt, ...)
{
	char buf[128];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > 0 && vt->cb.write)
		vt->cb.write(vt->cb.ud, buf, MIN((size_t)n, sizeof buf - 1));
}

static int charwidth_libc(uint32_t cp)
{
	int w = wcwidth((wchar_t)cp);
	return w < 0 ? 1 : w;
}

/* The BMP's widths, two bits a code point, a block of 256 filled from wcwidth()
 * when first asked: wcwidth for every code point from U+0300 cost CJK text a
 * fifth. Filled after mwt's setlocale, as wcwidth answers for the locale. */
static uint8_t width_tab[0x10000 / 4];
static uint8_t width_have[0x10000 / 256 / 8];

static int charwidth(uint32_t cp)
{
	if (cp < 0x300)
		return 1;
	if (cp > 0xffff)
		return charwidth_libc(cp);
	unsigned blk = cp >> 8;
	if (!(width_have[blk >> 3] & 1u << (blk & 7))) {
		for (uint32_t c = blk << 8; c < (blk + 1) << 8; c++)
			width_tab[c >> 2] |= (uint8_t)(charwidth_libc(c) << (c & 3) * 2);
		width_have[blk >> 3] |= (uint8_t)(1u << (blk & 7));
	}
	return width_tab[cp >> 2] >> (cp & 3) * 2 & 3;
}

static struct cell blank(const struct vt *vt)
{
	/* background color erase: erased cells take the current bg */
	return (struct cell){ ' ', COL_DEFAULT_FG, vt->c.tmpl.bg, 0 };
}

static void fill(struct cell *l, int from, int to, struct cell b)
{
	for (int i = from; i < to; i++)
		l[i] = b;
}

/* A row counts the cells from its left that may differ from a default blank
 * (row.used); every cell from there on is one. Scrolling and erasing then only
 * touch what was written, so a short line costs the same on a wide screen as on a
 * narrow one. It is an upper bound: writing raises it, erasing to the end of the
 * row with a default blank lowers it. It is never below row.wrap. */
static bool is_default_blank(struct cell c)
{
	return c.cp == ' ' && c.fg == COL_DEFAULT_FG && c.bg == COL_DEFAULT_BG && !c.attr;
}

/* A blank as the erase that set a row's tail left it: in that colour, and
 * not written. It is none of a row's content where it ends the row. A blank
 * a program wrote is, in the default colours too, where nothing shows of it:
 * a terminal of another width shows it given the same bytes (REFLOW.md 3.1,
 * D1). */
static bool is_tail_blank(struct cell c, uint32_t tail)
{
	return c.cp == ' ' && c.fg == COL_DEFAULT_FG && c.bg == tail && !c.attr;
}

/* How many of the len cells of a row without a join are its content: up to
 * the last that is written, or is no blank of the row's tail */
static int content_len(const struct cell *l, int len, uint32_t tail)
{
	while (len > 0 && is_tail_blank(l[len - 1], tail))
		len--;
	return len;
}

/* the row has been written up to end (exclusive) */
static void touch(struct row *l, int end)
{
	if (l->used < end)
		l->used = end;
}

/* Cells [from, to) of the row are written or erased. With the last one before
 * the wrap among them the line ends in this row, as for a program that fills the
 * last column and then moves the cursor: only put() running into the end of the
 * row makes a line go on, and shift_right() keeps one that did. */
static void unwrap(struct row *l, int from, int to)
{
	if (from < l->wrap && l->wrap <= to)
		l->wrap = 0;
}

/* cells [from, to) of the row have been written */
static void wrote(struct row *l, int from, int to)
{
	unwrap(l, from, to);
	touch(l, to);
}

/* One cell, for wide-glyph repairs and the like. Not an erase to the row's
 * end, in the last column either: the tail stays (REFLOW.md E5). A character
 * printed over half a glyph at the row's end set it otherwise, against W2,
 * and REP another than the characters it stands for. */
static void set_cell(struct row *l, int x, struct cell c)
{
	l->c[x] = c;
	unwrap(l, x, x + 1);
	if (!is_default_blank(c))
		touch(l, x + 1);
}

/* fill() for a screen row of cols: a default blank is only written where the
 * row was written, the rest is blank already. An erase that takes in the last
 * column says what the row's end is erased in (row.tail, REFLOW.md E1): EL and
 * ED, ECH and the blanks of ICH that reach it, those DCH brings in, a new row. */
static void fill_row(struct row *l, int from, int to, struct cell b, int cols)
{
	unwrap(l, from, to);
	if (to == cols)
		l->tail = b.bg;
	if (!is_default_blank(b)) {
		fill(l->c, from, to, b);
		touch(l, to);
		return;
	}
	fill(l->c, from, MIN(to, l->used), b);
	if (to >= l->used && from < l->used)
		l->used = from;
}

static void free_screen(struct row **s, int rows)
{
	if (!s)
		return;
	for (int r = 0; r < rows; r++)
		free(s[r]);
	free(s);
}

/* a blank screen, or NULL with nothing left allocated */
static struct row **alloc_screen(int cols, int rows)
{
	struct row **s = calloc(rows, sizeof *s);
	if (!s)
		return NULL;
	struct cell b = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
	for (int r = 0; r < rows; r++) {
		s[r] = malloc(sizeof **s + (size_t)cols * sizeof *s[r]->c);
		if (!s[r]) {
			free_screen(s, rows);
			return NULL;
		}
		s[r]->used = s[r]->wrap = 0; /* nothing written, and it ends here */
		s[r]->tail = COL_DEFAULT_BG;
		fill(s[r]->c, 0, cols, b);
	}
	return s;
}

/* Screen rows a to b changed. dirty is by the rows shown: scrolled back by view,
 * screen row y is shown in row y + view, and the history above did not change. */
static void dirty_range(struct vt *vt, int a, int b)
{
	/* every line feed at the bottom marks the region: a loop was 29% of yes */
	a = MAX(a + vt->view, 0);
	b = MIN(b + vt->view, vt->rows - 1);
	if (a <= b)
		memset(vt->dirty + a, true, (size_t)(b - a + 1));
}

static void mark_row(struct vt *vt, int y)
{
	dirty_range(vt, y, y);
}

void vt_mark_all(struct vt *vt)
{
	memset(vt->dirty, true, (size_t)vt->rows);
}

/* break up wide glyphs that are partially overwritten in [x0, x1] */
static void fix_wide(struct vt *vt, struct row *l, int x0, int x1)
{
	if (x0 > 0 && (l->c[x0].attr & ATTR_WDUMMY))
		set_cell(l, x0 - 1, blank(vt));
	if (x1 + 1 < vt->cols && (l->c[x1].attr & ATTR_WIDE))
		set_cell(l, x1 + 1, blank(vt));
}

/* Before cells shift right from x: a glyph with x as its right half is split, so
 * both halves go. One with x as its left half moves as a whole, nothing to do. */
static void split_wide(struct vt *vt, struct row *l, int x)
{
	if (!(l->c[x].attr & ATTR_WDUMMY))
		return;
	if (x > 0)
		set_cell(l, x - 1, blank(vt));
	set_cell(l, x, blank(vt));
}

/* The row's cells from x on move n to the right, and those pushed past its end
 * are lost; the n cells at x are the caller's to write. A line that went on in
 * the next row still does: what is put in is part of it, and its wrap moves
 * along, to the end of the row at most. Not when the cells put in reach the
 * last one before the wrap: the caller writes them, and that ends the line as
 * any write there does (unwrap); nor when x is the right half of a wide glyph
 * that was the last one. Half a wide glyph left in the last column goes. */
static void shift_right(struct vt *vt, struct row *l, int x, int n)
{
	split_wide(vt, l, x);
	int wrap = l->wrap;
	memmove(&l->c[x + n], &l->c[x], (size_t)(vt->cols - x - n) * sizeof *l->c);
	touch(l, MIN(l->used + n, vt->cols));
	if (l->c[vt->cols - 1].attr & ATTR_WIDE)
		set_cell(l, vt->cols - 1, blank(vt));
	l->wrap = x < wrap ? MIN(wrap + n, vt->cols) : wrap;
}

static void row_changed(struct vt *vt, int y);

static void clear_line(struct vt *vt, int y, int x0, int x1)
{
	x0 = CLAMP(x0, 0, vt->cols - 1);
	x1 = CLAMP(x1, 0, vt->cols - 1);
	if (x0 > x1)
		return;
	fix_wide(vt, vt->line[y], x0, x1);
	fill_row(vt->line[y], x0, x1 + 1, blank(vt), vt->cols);
	/* erased to the end, the line ends (J3c): also one whose wrap is left of
	 * x0, behind a row whose line ends empty or where a window grown wider
	 * left it on the alternate screen */
	if (x1 == vt->cols - 1)
		vt->line[y]->wrap = 0;
	row_changed(vt, y);
	mark_row(vt, y);
}

/* ---- scrollback ------------------------------------------------------- */

/* The newest history line no longer goes on in the first row of the main
 * screen, which was erased: a selection or a URL over the two would join text
 * that is not one line any more. For the screen cleared (ED from its start,
 * RIS), not for a line erased, which readline does to the line it redraws.
 * And for a first row that is a new one, with the rows moved down (J5). */
static void hist_unwrap(struct vt *vt)
{
	if ((vt->mode & MODE_ALTSCREEN) || vt->hist_count == 0)
		return;
	int slot = (vt->hist_head - 1 + vt->hist_cap) % vt->hist_cap;
	vt->hist_wrap[slot] = 0;
}

/* Cells of row y of the screen shown have changed, or its tail has. Two joins
 * are kept right here, so that nothing has to be checked where one is read.
 *
 * The row's own: the cells behind it are no content, the one a wide glyph did
 * not fit in or what a resize left of a row whose line ends empty. With
 * anything else there the line ends in this row (REFLOW.md I2, J3b).
 *
 * That of the row before, on the main screen, which for the first row is the
 * history's newest: a join short of the last column is one the layout made
 * for what this row was, empty or beginning with a wide glyph. Once the row
 * has content, or begins with something else, it is a row of its own (I8,
 * J9). A join after the last column stays: a full row goes on into whatever
 * is written here (J6). The alternate screen keeps its joins (X1). */
static void row_changed(struct vt *vt, int y)
{
	struct row *l = vt->line[y];
	for (int k = l->wrap; l->wrap && k < l->used; k++)
		if (!is_tail_blank(l->c[k], l->tail))
			l->wrap = 0;
	if (vt->mode & MODE_ALTSCREEN)
		return;
	int *join;
	if (y > 0)
		join = &vt->pri[y - 1]->wrap;
	else if (vt->hist_count)
		join = &vt->hist_wrap[(vt->hist_head - 1 + vt->hist_cap) % vt->hist_cap];
	else
		return;
	if (!*join || *join == vt->cols)
		return;
	if (*join == vt->cols - 1 && (l->c[0].attr & ATTR_WIDE))
		return;
	if (l->wrap || content_len(l->c, l->used, l->tail))
		*join = 0;
}

static void push_hist(struct vt *vt, const struct row *row)
{
	if (vt->hist_cap <= 0)
		return;
	/* The row's content is kept, the blanks a program wrote at its end with
	 * it (H1), and the cells before a wrap whatever they are. Not the blanks
	 * of an erase behind it: they are there again where the row is shown.
	 * Those of an erase in a colour are kept, to the last column. A screen
	 * row is blank from row.used on, so the search starts there. */
	const struct cell *l = row->c;
	int len = row->tail != COL_DEFAULT_BG ? row->used :
		  MAX(content_len(l, row->used, row->tail), row->wrap);
	int slot = vt->hist_head;
	struct cell *dst = vt->hist[slot];
	/* a slot keeps its cells: once the history is full, a line that fits
	 * where the one it replaces was needs no realloc */
	if (!dst || len > vt->hist_room[slot]) {
		dst = realloc(dst, MAX(len, 1) * sizeof *dst);
		if (dst) {
			vt->hist[slot] = dst;
			vt->hist_room[slot] = MAX(len, 1);
		}
	}
	/* out of memory the line goes in empty: it still scrolled off, and
	 * skipping it would shift every line number a selection holds */
	if (dst)
		memcpy(dst, l, len * sizeof *dst);
	vt->hist_len[slot] = dst ? len : 0;
	vt->hist_wrap[slot] = dst ? row->wrap : 0;
	vt->hist_tail[slot] = dst ? row->tail : COL_DEFAULT_BG;
	vt->hist_head = (slot + 1) % vt->hist_cap;
	if (vt->hist_count < vt->hist_cap)
		vt->hist_count++;
	vt->scrolled++;
	if (vt->view > 0) { /* keep the viewed text in place */
		int v = vt->view;
		vt->view = MIN(v + 1, vt->hist_count);
		if (vt->view == v) /* at the top of a full history: it moves */
			vt_mark_all(vt);
	}
}

/* vt_abs_line, and in *wrap the cells before the line's wrap, as row.wrap */
static const struct cell *abs_line(const struct vt *vt, int64_t abs, int *len, int *wrap,
				   uint32_t *tail)
{
	int64_t rel = abs - (int64_t)vt->scrolled;
	if (rel >= 0) {
		if (rel >= vt->rows)
			goto none;
		*len = vt->cols;
		*wrap = vt->line[rel]->wrap;
		*tail = vt->line[rel]->tail;
		return vt->line[rel]->c;
	}
	int64_t i = -rel - 1;
	if (i >= vt->hist_count)
		goto none;
	int slot = (int)((vt->hist_head - 1 - i + vt->hist_cap) % vt->hist_cap);
	*len = vt->hist_len[slot];
	*wrap = vt->hist_wrap[slot];
	*tail = vt->hist_tail[slot];
	return vt->hist[slot];
none:
	*len = *wrap = 0;
	*tail = COL_DEFAULT_BG;
	return NULL;
}

const struct cell *vt_abs_line(const struct vt *vt, int64_t abs, int *len)
{
	int wrap;
	uint32_t tail;
	return abs_line(vt, abs, len, &wrap, &tail);
}

uint32_t vt_abs_tail(const struct vt *vt, int64_t abs)
{
	int n, wrap;
	uint32_t tail;
	abs_line(vt, abs, &n, &wrap, &tail);
	return tail;
}

const struct cell *vt_view_line(const struct vt *vt, int r, int *len)
{
	return vt_abs_line(vt, (int64_t)vt->scrolled - vt->view + r, len);
}

int vt_abs_wraps(const struct vt *vt, int64_t abs)
{
	int n, wrap;
	uint32_t tail;
	abs_line(vt, abs, &n, &wrap, &tail);
	return wrap;
}

int64_t vt_abs_first(const struct vt *vt)
{
	return (int64_t)vt->scrolled - vt->hist_count;
}

void vt_scroll_view(struct vt *vt, int delta)
{
	if (vt->mode & MODE_ALTSCREEN)
		return;
	int v = CLAMP(vt->view + delta, 0, vt->hist_count);
	if (v != vt->view) {
		vt->view = v;
		vt_mark_all(vt);
	}
}

/* ---- scrolling -------------------------------------------------------- */

/* Rows orig to vt.bot move up by n; the first n go, into the history from
 * the top of the main screen, and n new ones come in below.
 *
 * A row goes on only in the row that followed it before (REFLOW.md J5). The
 * last that moves is followed by a new row: its line ends there, unless
 * put() has just wrapped it and writes on in that row (goes_on). The row
 * above those that move is followed by another than before; not the
 * history's newest, which the rows that go there follow as they did. */
static void scroll_up(struct vt *vt, int orig, int n, bool goes_on)
{
	if (orig < vt->top || orig > vt->bot)
		return;
	n = CLAMP(n, 0, vt->bot - orig + 1);
	if (!n)
		return;
	if (!goes_on)
		vt->line[vt->bot]->wrap = 0;
	if (orig > 0)
		vt->line[orig - 1]->wrap = 0;
	bool hist = orig == 0 && !(vt->mode & MODE_ALTSCREEN);
	if (hist)
		for (int i = 0; i < n; i++)
			push_hist(vt, vt->line[i]);
	struct row *tmp[n];
	memcpy(tmp, &vt->line[orig], n * sizeof *tmp);
	memmove(&vt->line[orig], &vt->line[orig + n],
		(vt->bot - orig + 1 - n) * sizeof *tmp);
	memcpy(&vt->line[vt->bot - n + 1], tmp, n * sizeof *tmp);
	for (int r = vt->bot - n + 1; r <= vt->bot; r++)
		fill_row(vt->line[r], 0, vt->cols, blank(vt), vt->cols);
	/* scrolled back, push_hist moved the view: the rows below the region
	 * did not change, but they are shown n rows lower now */
	dirty_range(vt, orig, hist && vt->view > 0 ? vt->rows - 1 : vt->bot);
}

/* Rows orig to vt.bot move down by n; the last n go, and n new ones come in
 * above. The row above them is followed by a new one then, and so is the
 * history's newest above the first row; the last that stays by what is
 * below the region, or by nothing: their lines end (REFLOW.md J5). */
static void scroll_down(struct vt *vt, int orig, int n)
{
	if (orig < vt->top || orig > vt->bot)
		return;
	n = CLAMP(n, 0, vt->bot - orig + 1);
	if (!n)
		return;
	if (orig > 0)
		vt->line[orig - 1]->wrap = 0;
	else
		hist_unwrap(vt);
	if (n <= vt->bot - orig)
		vt->line[vt->bot - n]->wrap = 0;
	struct row *tmp[n];
	memcpy(tmp, &vt->line[vt->bot - n + 1], n * sizeof *tmp);
	memmove(&vt->line[orig + n], &vt->line[orig],
		(vt->bot - orig + 1 - n) * sizeof *tmp);
	memcpy(&vt->line[orig], tmp, n * sizeof *tmp);
	for (int r = orig; r < orig + n; r++)
		fill_row(vt->line[r], 0, vt->cols, blank(vt), vt->cols);
	dirty_range(vt, orig, vt->bot);
}

/* wrapped: put() has set the wrap of the cursor's row and writes on in the
 * row this gives it */
static void index_down(struct vt *vt, bool wrapped)
{
	if (vt->c.y == vt->bot)
		scroll_up(vt, vt->top, 1, wrapped);
	else if (vt->c.y < vt->rows - 1)
		vt->c.y++;
}

static void reverse_index(struct vt *vt)
{
	if (vt->c.y == vt->top)
		scroll_down(vt, vt->top, 1);
	else if (vt->c.y > 0)
		vt->c.y--;
}

/* ---- cursor ----------------------------------------------------------- */

static void moveto(struct vt *vt, int x, int y)
{
	int lo = 0, hi = vt->rows - 1;
	if (vt->c.origin) {
		lo = vt->top;
		hi = vt->bot;
	}
	vt->c.x = CLAMP(x, 0, vt->cols - 1);
	vt->c.y = CLAMP(y, lo, hi);
	vt->c.wrapnext = false;
}

/* the row origin mode counts from */
static int origin_row(const struct vt *vt)
{
	return vt->c.origin ? vt->top : 0;
}

/* absolute position, honoring origin mode */
static void moveto_abs(struct vt *vt, int x, int y)
{
	moveto(vt, x, y + origin_row(vt));
}

/* the cursor to column x of its row after writing: past the last one it stays
 * there, with a wrap pending if autowrap is on */
static void advance(struct vt *vt, int x, bool wrap)
{
	vt->c.x = x;
	if (x >= vt->cols) {
		vt->c.x = vt->cols - 1;
		vt->c.wrapnext = wrap;
	}
}

static void save_cursor(struct vt *vt)
{
	if (vt->mode & MODE_ALTSCREEN) {
		vt->saved_alt = vt->c;
	} else {
		vt->saved_pri = vt->c;
		vt->pri_saved = true;
	}
}

static void restore_cursor(struct vt *vt)
{
	vt->c = (vt->mode & MODE_ALTSCREEN) ? vt->saved_alt : vt->saved_pri;
	vt->c.x = MIN(vt->c.x, vt->cols - 1);
	vt->c.y = MIN(vt->c.y, vt->rows - 1);
}

static void default_cursor(struct vt_cursor *c)
{
	memset(c, 0, sizeof *c);
	c->tmpl = (struct cell){ ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
}

static void reset_tabs(struct vt *vt)
{
	for (int i = 0; i < vt->cols; i++)
		vt->tabs[i] = i && i % 8 == 0;
}

static void tab(struct vt *vt, int n)
{
	int x = vt->c.x;
	if (n > 0) {
		while (n-- && x < vt->cols - 1)
			while (++x < vt->cols - 1 && !vt->tabs[x])
				;
	} else {
		while (n++ && x > 0)
			while (--x > 0 && !vt->tabs[x])
				;
		vt->c.wrapnext = false;
	}
	/* forward, a wrap that is pending stays so, as in xterm 407: the cursor
	 * is in the last column and goes nowhere */
	vt->c.x = x;
}

/* ---- screens and reset ------------------------------------------------ */

static void set_altscreen(struct vt *vt, bool on, bool clear)
{
	if (!!(vt->mode & MODE_ALTSCREEN) == on)
		return;
	if (on) {
		/* the place vt_resize keeps on the main screen meanwhile: the cursor
		 * goes on to the alternate one, and only 1049 saves it on the way */
		vt->pri_x = vt->c.x + vt->c.wrapnext;
		vt->pri_y = vt->c.y;
		vt->mode |= MODE_ALTSCREEN;
		vt->line = vt->alt;
		if (clear)
			for (int r = 0; r < vt->rows; r++)
				fill_row(vt->alt[r], 0, vt->cols, blank(vt), vt->cols);
	} else {
		vt->mode &= ~MODE_ALTSCREEN;
		vt->line = vt->pri;
	}
	vt->view = 0;
	vt_mark_all(vt);
}

static void soft_reset(struct vt *vt)
{
	vt->mode &= ~(MODE_INSERT | MODE_HIDECURSOR | MODE_APPCURSOR |
		      MODE_APPKEYPAD);
	vt->cursor_style = CURSOR_BLOCK; /* as xterm, so is2 undoes a bar vim left */
	mark_row(vt, vt->c.y);
	vt->mode |= MODE_AUTOWRAP;
	vt->top = 0;
	vt->bot = vt->rows - 1;
	int x = vt->c.x, y = vt->c.y;
	bool wrapnext = vt->c.wrapnext; /* kept, as xterm */
	default_cursor(&vt->c);
	vt->c.x = x;
	vt->c.y = y;
	vt->c.wrapnext = wrapnext;
	/* only the saved cursor of the screen it is on, as xterm */
	struct vt_cursor *saved = vt->mode & MODE_ALTSCREEN ? &vt->saved_alt : &vt->saved_pri;
	*saved = vt->c;
	saved->x = saved->y = 0; /* and keeps a pending wrap, as xterm */
	if (saved == &vt->saved_pri)
		vt->pri_saved = false;
}

void vt_reset(struct vt *vt)
{
	set_altscreen(vt, false, false);
	hist_unwrap(vt);
	vt->mode = MODE_AUTOWRAP;
	vt->cursor_style = CURSOR_BLOCK;
	default_cursor(&vt->c);
	vt->saved_pri = vt->saved_alt = vt->c;
	vt->pri_saved = false;
	vt->top = 0;
	vt->bot = vt->rows - 1;
	vt->state = ST_GROUND;
	vt->utf8_need = 0;
	vt->lastcp = 0;
	reset_tabs(vt);
	for (int r = 0; r < vt->rows; r++) {
		fill_row(vt->pri[r], 0, vt->cols, blank(vt), vt->cols);
		fill_row(vt->alt[r], 0, vt->cols, blank(vt), vt->cols);
	}
	vt->view = 0;
	vt_mark_all(vt);
}

struct vt *vt_new(int cols, int rows, int histsize, const struct vt_callbacks *cb)
{
	struct vt *vt = calloc(1, sizeof *vt);
	if (!vt)
		return NULL;
	vt->cols = CLAMP(cols, 1, VT_MAX_COLS);
	vt->rows = CLAMP(rows, 1, VT_MAX_ROWS);
	vt->pri = alloc_screen(vt->cols, vt->rows);
	vt->alt = alloc_screen(vt->cols, vt->rows);
	vt->line = vt->pri;
	vt->dirty = calloc(vt->rows, sizeof *vt->dirty);
	vt->tabs = calloc(vt->cols, sizeof *vt->tabs);
	vt->hist_cap = MAX(histsize, 0);
	if (vt->hist_cap) {
		vt->hist = calloc(vt->hist_cap, sizeof *vt->hist);
		vt->hist_len = calloc(vt->hist_cap, sizeof *vt->hist_len);
		vt->hist_room = calloc(vt->hist_cap, sizeof *vt->hist_room);
		vt->hist_wrap = calloc(vt->hist_cap, sizeof *vt->hist_wrap);
		vt->hist_tail = calloc(vt->hist_cap, sizeof *vt->hist_tail);
	}
	if (!vt->pri || !vt->alt || !vt->dirty || !vt->tabs ||
	    (vt->hist_cap && (!vt->hist || !vt->hist_len || !vt->hist_room || !vt->hist_wrap ||
			      !vt->hist_tail))) {
		vt_free(vt);
		return NULL;
	}
	if (cb)
		vt->cb = *cb;
	vt_reset(vt);
	return vt;
}

void vt_free(struct vt *vt)
{
	if (!vt)
		return;
	free_screen(vt->pri, vt->rows);
	free_screen(vt->alt, vt->rows);
	for (int i = 0; vt->hist && i < vt->hist_cap; i++)
		free(vt->hist[i]);
	free(vt->hist);
	free(vt->hist_len);
	free(vt->hist_room);
	free(vt->hist_wrap);
	free(vt->hist_tail);
	free(vt->dirty);
	free(vt->tabs);
	free(vt);
}

/* a new screen holding what fits of old, which is left as it is: its row shift
 * as the first one, or with -shift rows left blank above it for the caller.
 * Cut to cols and not laid out anew: for the alternate screen, which its
 * program draws again, and for the main one where only the rows change. */
static struct row **copy_screen(const struct vt *vt, struct row **old, int shift,
				int cols, int rows)
{
	struct row **s = alloc_screen(cols, rows);
	if (!s)
		return NULL;
	struct cell b = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
	for (int r = MAX(0, -shift); r < rows && r + shift < vt->rows; r++) {
		const struct row *o = old[r + shift];
		s[r]->used = MIN(o->used, cols);
		memcpy(s[r]->c, o->c, s[r]->used * sizeof *o->c);
		/* a line cut before its wrap ends there */
		s[r]->wrap = o->wrap <= cols ? o->wrap : 0;
		/* kept at this width and in a row that is cut; what a wider
		 * window adds on the alternate screen are default blanks, an
		 * erase of the new cells: a join stays where it was, unless
		 * the cells behind it are of another colour than that (X1) */
		s[r]->tail = cols > vt->cols ? COL_DEFAULT_BG : o->tail;
		for (int k = s[r]->wrap; s[r]->wrap && k < s[r]->used; k++)
			if (!is_tail_blank(s[r]->c[k], s[r]->tail))
				s[r]->wrap = 0;
		if (cols < vt->cols && (s[r]->c[cols - 1].attr & ATTR_WIDE))
			s[r]->c[cols - 1] = b;
	}
	/* the row a last row went on into was cut off: the line ends there, or
	 * what is written below it later would be joined to it */
	if (rows + shift < vt->rows)
		s[rows - 1]->wrap = 0;
	return s;
}

/* The slot of the history line back lines up, 0 for the newest. */
static int hist_slot(const struct vt *vt, int back)
{
	return (vt->hist_head - 1 - back + vt->hist_cap) % vt->hist_cap;
}

/* ---- reflow ----------------------------------------------------------- */

/* A place on the old main screen to find again on the new one: the cursor, the
 * saved one, the first row. Rows count from the oldest history line. */
struct mark {
	int g, x;       /* x behind the row's text is as far behind its end; for
	                 * the cursor there is none: the cells before it are text */
	int64_t row;    /* where it is now */
	int col;
	bool pend;      /* right behind a row that is full: a wrap is pending */
};

/* The lines of the history and the main screen laid out for another width:
 * walked once to find the marks, and again to fill the rows in, those from top
 * on into scr and the hist_n before them into the history. */
struct reflow {
	const struct vt *vt;
	int cols, rows;
	struct mark *m;
	int nm;
	int64_t row;        /* the row being filled */
	int x;              /* cells in it */
	bool out;           /* the second walk */
	int64_t top;
	struct cell *buf;   /* the row being filled, cols cells */
	struct row **scr;
	struct cell **hist;
	int *hist_len, *hist_room, *hist_wrap;
	uint32_t *hist_tail;
	int hist_n;
	bool nomem;
};

/* Old row g, the history's first, as it is kept: its cells, how many, its
 * wrap and its tail. */
static const struct cell *old_cells(const struct vt *vt, int g, int *len, int *wrap,
				    uint32_t *tail)
{
	const struct cell *l;
	if (g < vt->hist_count) {
		int slot = hist_slot(vt, vt->hist_count - 1 - g);
		l = vt->hist[slot];
		*len = vt->hist_len[slot];
		*wrap = vt->hist_wrap[slot];
		*tail = vt->hist_tail[slot];
	} else {
		const struct row *r = vt->pri[g - vt->hist_count];
		l = r->c;
		*len = r->used;
		*wrap = r->wrap;
		*tail = r->tail;
	}
	return l;
}

/* The tail of the line old row g is in: its last row's (REFLOW.md D8). */
static uint32_t line_tail(const struct vt *vt, int g)
{
	int len, wrap;
	uint32_t tail;
	while (old_cells(vt, g, &len, &wrap, &tail), wrap && g + 1 < vt->hist_count + vt->rows)
		g++;
	return tail;
}

/* Old row g: its cells, in *n how many of them are text, and in *more whether
 * the line goes on in the next row: where the row says so (row.wrap), and
 * the last one never.
 *
 * The blanks at the end of a line that its tail says are there anyway are no
 * text: those of the erase, in its colour and not written (row.tail). They
 * are not laid out but put behind the line's text again, up to the new last
 * column: counted as text, a row erased in a colour took two rows one column
 * narrower. Which they are the row says, not where they happen to be: blanks
 * of an erase in another colour, or of one that ended before the last
 * column, are text wherever another width puts them. So are blanks a program
 * wrote, in the default colours too (content_len()). The first keep cells are
 * text whatever they hold: the cells between the text and a cursor moved
 * behind it, without which the cursor had no place to be, and those before
 * a cursor in a row that holds nothing: clamped into a narrower row, it was
 * not back in its column in a wider one. */
static const struct cell *old_row(const struct vt *vt, int g, int keep, int *n, bool *more)
{
	int len, wrap;
	uint32_t tail;
	const struct cell *l = old_cells(vt, g, &len, &wrap, &tail);
	*more = wrap && g + 1 < vt->hist_count + vt->rows;
	if (wrap) {
		*n = wrap;
		return l;
	}
	*n = MAX(content_len(l, len, tail), keep);
	return l;
}

/* The row being filled is done: the line goes on after wrap cells, or ends
 * with 0. tail is the line's, and every row of it has it (REFLOW.md L5): in
 * the cells its text leaves, the one a wide glyph did not fit in too (L2),
 * and as the row's in any case, for the text may leave none. Out of memory
 * the walk goes on, for nothing. */
static void reflow_row_done(struct reflow *rf, int wrap, uint32_t tail)
{
	int64_t r = rf->row - rf->top;
	if (rf->out && r >= -rf->hist_n && r < rf->rows && !rf->nomem) {
		for (; tail != COL_DEFAULT_BG && rf->x < rf->cols; rf->x++)
			rf->buf[rf->x] = (struct cell){ ' ', COL_DEFAULT_FG, tail, 0 };
		size_t size = (size_t)rf->x * sizeof *rf->buf;
		if (r < 0) {
			int k = (int)(r + rf->hist_n);
			rf->hist[k] = malloc(size ? size : sizeof *rf->buf);
			if (rf->hist[k]) {
				memcpy(rf->hist[k], rf->buf, size);
				rf->hist_len[k] = rf->x;
				rf->hist_room[k] = MAX(rf->x, 1);
				rf->hist_wrap[k] = wrap;
				rf->hist_tail[k] = tail;
			} else {
				rf->nomem = true;
			}
		} else {
			struct row *l = rf->scr[r];
			memcpy(l->c, rf->buf, size);
			l->used = rf->x;
			l->wrap = wrap;
			l->tail = tail;
		}
	}
	rf->row++;
	rf->x = 0;
}

/* The marks of row g in its cells from lo up to hi are in the row being filled:
 * at col and counted on from there, or with step 0 all at col, for a glyph that
 * is gone. Behind a row that is full, col its width, they wait in its last
 * column for the wrap, as after the character that filled it; further behind
 * text that leaves room, they are in the last column at most. */
static void reflow_mark(struct reflow *rf, int g, int lo, int hi, int col, int step)
{
	for (int k = 0; k < rf->nm; k++) {
		struct mark *m = &rf->m[k];
		if (m->g != g || m->x < lo || m->x >= hi)
			continue;
		m->row = rf->row;
		m->col = MIN(col + step * (m->x - lo), rf->cols - 1);
		m->pend = col == rf->cols;
	}
}

static void reflow_walk(struct reflow *rf)
{
	static const struct cell blankc = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
	const struct vt *vt = rf->vt;
	int cols = rf->cols, total = vt->hist_count + vt->rows;
	bool joined = false; /* the row before goes on in this one */
	uint32_t tail = COL_DEFAULT_BG; /* of the line being laid out */
	rf->row = 0;
	rf->x = 0;
	for (int g = 0; g < total && !rf->nomem; g++) {
		/* the screen is full: what follows fell off its end */
		if (rf->out && rf->row >= rf->top + rf->rows)
			break;
		int n;
		bool more, marked = false;
		if (!joined)
			tail = line_tail(vt, g);
		/* m[0] is the cursor: the cells before it are text */
		const struct cell *l = old_row(vt, g, rf->m[0].g == g ? MIN(rf->m[0].x, vt->cols) : 0,
					       &n, &more);
		for (int k = 0; k < rf->nm; k++) {
			struct mark *m = &rf->m[k];
			/* In a row that goes on, a wrap pending behind its last column
			 * is the first cell of the next row, where the next character
			 * goes: the cursor came back to it after the wrap (DECRC). What
			 * else is behind its text, the cell a wide glyph did not fit
			 * in, is its last cell's. */
			if (m->g == g && more && m->x >= vt->cols) {
				m->g = g + 1;
				m->x = 0;
			} else if (m->g == g && more && m->x >= n) {
				m->x = n - 1;
			}
			marked |= m->g == g;
		}
		/* A row with no text that the line went on into, erased since or
		 * holding the blanks it ended with, stays a row: the text goes on
		 * into it from where it ends now, in the middle of a row too, as on
		 * the alternate screen after a wider window. Left out, all below it
		 * moved up a row with every other width. */
		if (joined && !more && !n && rf->x)
			reflow_row_done(rf, rf->x, tail);
		joined = more;
		for (int i = 0; i < n; i++) {
			struct cell c = l[i];
			int w = (c.attr & ATTR_WIDE) && i + 1 < n && (l[i + 1].attr & ATTR_WDUMMY) ? 2 : 1;
			/* half a wide glyph alone, which nothing should leave: a blank */
			if (w == 1 && (c.attr & (ATTR_WIDE | ATTR_WDUMMY)))
				c = blankc;
			/* One column has no room for a wide glyph, as put() has none: it
			 * goes, and a mark on it is behind what came before it. */
			if (w > cols) {
				if (marked)
					reflow_mark(rf, g, i, i + w, rf->x, 0);
				i += w - 1;
				continue;
			}
			if (rf->x + w > cols)
				reflow_row_done(rf, rf->x, tail);
			if (marked)
				reflow_mark(rf, g, i, i + w, rf->x, 1);
			if (rf->out) {
				rf->buf[rf->x] = c;
				if (w == 2)
					rf->buf[rf->x + 1] = l[i + 1];
			}
			rf->x += w;
			i += w - 1;
		}
		if (more)
			continue;
		if (marked)
			reflow_mark(rf, g, n, INT_MAX, rf->x, 1);
		reflow_row_done(rf, 0, tail);
	}
}

static void reflow_free(const struct vt *vt, struct reflow *rf)
{
	free_screen(rf->scr, rf->rows);
	for (int i = 0; rf->hist && i < vt->hist_cap; i++)
		free(rf->hist[i]);
	free(rf->hist);
	free(rf->hist_len);
	free(rf->hist_room);
	free(rf->hist_wrap);
	free(rf->hist_tail);
	free(rf->buf);
	rf->scr = NULL;
	rf->hist = NULL;
	rf->hist_len = rf->hist_room = rf->hist_wrap = NULL;
	rf->hist_tail = NULL;
	rf->buf = NULL;
}

/* The last row of the main screen that has content, or a tail in a colour;
 * the cursor's row cy if none comes after it (REFLOW.md 4.6, e0) */
static int screen_end(const struct vt *vt, int cy)
{
	int e = vt->rows - 1;
	while (e > cy && !vt->pri[e]->wrap &&
	       !content_len(vt->pri[e]->c, vt->pri[e]->used, vt->pri[e]->tail) &&
	       vt->pri[e]->tail == COL_DEFAULT_BG)
		e--;
	return e;
}

/* The main screen and the history for cols by rows, in *rf, with vt left as it
 * is; false when out of memory, and then nothing is allocated. m[0] is the
 * cursor, or the place it left behind for the alternate screen, m[1] the saved
 * cursor; m[2] is set to the first row of the screen, m[3] to the end of its
 * text: the last row with any or with a colour it was erased in, the cursor's
 * if none comes after it.
 *
 * Which rows the screen shows: those it showed, from its first row on. Text
 * that reached the last row still does, as lines come back from the history
 * where wider rows take fewer; text that ended further up moves with its rows
 * and nothing comes back, so that what a program cleared stays cleared. A
 * taller window takes as many lines back as it grew, as without another width.
 * Then what no longer fits goes into the history from the top, as far as the
 * cursor stays on the screen: past that, what is below it is lost, as it is in
 * a shorter window. */
static bool reflow(const struct vt *vt, int cols, int rows, struct reflow *rf, struct mark *m)
{
	int e = screen_end(vt, m[0].g - vt->hist_count);
	*rf = (struct reflow){ .vt = vt, .cols = cols, .rows = rows, .m = m, .nm = 4 };
	m[2] = (struct mark){ .g = vt->hist_count };
	m[3] = (struct mark){ .g = vt->hist_count + e, .x = vt->cols };
	reflow_walk(rf);
	int64_t top = m[2].row, end = m[3].row;
	if (e == vt->rows - 1)
		top = MIN(top, end - e);
	top = MAX(top - MAX(0, rows - vt->rows), 0);
	if (end - top > rows - 1)
		top = end - (rows - 1);
	top = MIN(top, m[0].row);
	rf->top = top;
	rf->hist_n = (int)MIN(top, vt->hist_cap);
	rf->scr = alloc_screen(cols, rows);
	rf->buf = malloc((size_t)cols * sizeof *rf->buf);
	if (vt->hist_cap) {
		rf->hist = calloc(vt->hist_cap, sizeof *rf->hist);
		rf->hist_len = calloc(vt->hist_cap, sizeof *rf->hist_len);
		rf->hist_room = calloc(vt->hist_cap, sizeof *rf->hist_room);
		rf->hist_wrap = calloc(vt->hist_cap, sizeof *rf->hist_wrap);
		rf->hist_tail = calloc(vt->hist_cap, sizeof *rf->hist_tail);
	}
	rf->nomem = !rf->scr || !rf->buf ||
		    (vt->hist_cap && (!rf->hist || !rf->hist_len || !rf->hist_room ||
				      !rf->hist_wrap || !rf->hist_tail));
	if (!rf->nomem) {
		rf->out = true;
		reflow_walk(rf);
		/* the row the last one went on into fell off the screen's end: the
		 * line ends there, as in copy_screen() */
		rf->scr[rows - 1]->wrap = 0;
	}
	free(rf->buf);
	rf->buf = NULL;
	if (rf->nomem)
		reflow_free(vt, rf);
	return !rf->nomem;
}

static void clamp_cursor(struct vt_cursor *c, int cols, int rows)
{
	c->x = CLAMP(c->x, 0, cols - 1);
	c->y = CLAMP(c->y, 0, rows - 1);
	c->wrapnext = false;
}

bool vt_resize(struct vt *vt, int cols, int rows)
{
	cols = CLAMP(cols, 1, VT_MAX_COLS);
	rows = CLAMP(rows, 1, VT_MAX_ROWS);
	if (cols == vt->cols && rows == vt->rows)
		return true;
	bool alt = vt->mode & MODE_ALTSCREEN;
	/* The main screen's cursor row: behind the alternate screen, where it was
	 * when that one took over. Not saved_pri: 47 and 1047 save no cursor, and
	 * it holds what DECSC last left, the top row if nothing. */
	int *py = alt ? &vt->pri_y : &vt->c.y;
	struct vt_cursor *ac = alt ? &vt->c : &vt->saved_alt;
	int ashift = MAX(0, ac->y - (rows - 1));

	/* everything new first, so a failure leaves the terminal as it was */
	bool anew = cols != vt->cols;
	int pshift = 0, pull = 0;
	struct reflow rf = { 0 };
	struct mark m[4];
	struct row **npri;
	if (anew) {
		/* Another width: the lines of the main screen and of the history are
		 * laid out for it, behind the alternate screen too. A pending wrap
		 * is the place behind the cursor's cell. */
		const struct vt_cursor *s = &vt->saved_pri;
		m[0] = (struct mark){ .g = vt->hist_count + *py,
				      .x = alt ? vt->pri_x : vt->c.x + vt->c.wrapnext };
		m[1] = (struct mark){ .g = vt->hist_count + s->y, .x = s->x + s->wrapnext };
		npri = reflow(vt, cols, rows, &rf, m) ? rf.scr : NULL;
	} else {
		/* Another height alone: the rows stay as they are, and which of
		 * them the screen shows is decided as with another width
		 * (REFLOW.md 4.6, V1 to V5). In a shorter window the end of the
		 * text stays on the screen and rows go into the history from the
		 * top, as far as the cursor's row stays too; what is below the
		 * screen then is lost. */
		pshift = MIN(MAX(0, screen_end(vt, *py) - (rows - 1)), *py);
		/* And a taller main screen takes the newest ones back above its
		 * text, as xterm does: what a shorter window pushed off returns with
		 * the window. They fit: the history is as wide as the screen. Never
		 * both: an end that fit before fits a taller screen. */
		pull = CLAMP(rows - vt->rows, 0, vt->hist_count);
		npri = copy_screen(vt, vt->pri, pshift - pull, cols, rows);
	}
	struct row **nalt = copy_screen(vt, vt->alt, ashift, cols, rows);
	bool *ndirty = calloc(rows, sizeof *ndirty);
	bool *ntabs = calloc(cols, sizeof *ntabs);
	if (!npri || !nalt || !ndirty || !ntabs) {
		if (anew)
			reflow_free(vt, &rf);
		else
			free_screen(npri, rows);
		free_screen(nalt, rows);
		free(ndirty);
		free(ntabs);
		return false;
	}
	bool pend[2] = { false, false };
	if (anew) {
		/* the new history in the old one's place; the lines count on from
		 * where they did, past those that fell off its far end */
		int64_t first = vt_abs_first(vt);
		for (int i = 0; i < vt->hist_cap; i++)
			free(vt->hist[i]);
		free(vt->hist);
		free(vt->hist_len);
		free(vt->hist_room);
		free(vt->hist_wrap);
		free(vt->hist_tail);
		vt->hist = rf.hist;
		vt->hist_len = rf.hist_len;
		vt->hist_room = rf.hist_room;
		vt->hist_wrap = rf.hist_wrap;
		vt->hist_tail = rf.hist_tail;
		vt->hist_count = rf.hist_n;
		if (vt->hist_cap) {
			vt->hist_head = rf.hist_n % vt->hist_cap;
			vt->scrolled = (uint64_t)(first + rf.top);
		}
		*py = (int)(m[0].row - rf.top);
		if (alt) {
			vt->pri_x = m[0].col + m[0].pend;
		} else {
			vt->c.x = m[0].col;
			pend[0] = m[0].pend;
		}
		/* A saved cursor whose text is no longer on the screen: the nearest
		 * row. None was saved since a reset: DECRC goes home, in a window
		 * of this size as in one that had it all along (C5). */
		int64_t sy = m[1].row - rf.top;
		if (vt->pri_saved) {
			vt->saved_pri.x = m[1].col;
			vt->saved_pri.y = (int)CLAMP(sy, 0, rows - 1);
			pend[1] = m[1].pend && sy == vt->saved_pri.y;
		}
	} else {
		for (int i = 0; i < pshift; i++)
			push_hist(vt, vt->pri[i]);
		/* the lines taken back, the oldest in the top row: wrap and all, and
		 * with no memory needed, so nothing here can fail; then out of the
		 * history */
		for (int i = 0; i < pull; i++) {
			int slot = hist_slot(vt, pull - 1 - i);
			struct row *l = npri[i];
			l->used = vt->hist_len[slot];
			if (l->used)
				memcpy(l->c, vt->hist[slot], l->used * sizeof *l->c);
			l->wrap = vt->hist_wrap[slot];
			l->tail = vt->hist_tail[slot];
		}
		if (pull) {
			vt->hist_head = hist_slot(vt, pull - 1);
			vt->hist_count -= pull;
			vt->scrolled -= pull; /* the lines count from the same place still */
		}
		*py += pull - pshift;
		/* a saved cursor moves with its screen, to the top row at most */
		if (vt->pri_saved)
			vt->saved_pri.y += pull - MIN(pshift, vt->saved_pri.y);
	}
	free_screen(vt->pri, vt->rows);
	free_screen(vt->alt, vt->rows);
	vt->pri = npri;
	vt->alt = nalt;
	vt->line = alt ? vt->alt : vt->pri;
	ac->y -= ashift;
	/* behind the main screen ac is the alternate one's saved cursor and has moved */
	if (alt)
		vt->saved_alt.y -= MIN(ashift, vt->saved_alt.y);

	free(vt->dirty);
	free(vt->tabs);
	vt->dirty = ndirty;
	vt->tabs = ntabs;
	vt->cols = cols;
	vt->rows = rows;
	reset_tabs(vt);
	vt->top = 0;
	vt->bot = rows - 1;
	/* A wrap that is pending stays so in a window of the same width: the cursor
	 * is in the last column still. Dropped, the next character wrote over the
	 * one before it. */
	bool wn[3] = { vt->c.wrapnext, vt->saved_pri.wrapnext, vt->saved_alt.wrapnext };
	clamp_cursor(&vt->c, cols, rows);
	clamp_cursor(&vt->saved_pri, cols, rows);
	clamp_cursor(&vt->saved_alt, cols, rows);
	if (!anew) {
		vt->c.wrapnext = wn[0];
		vt->saved_pri.wrapnext = wn[1];
		vt->saved_alt.wrapnext = wn[2];
	} else if (vt->mode & MODE_AUTOWRAP) {
		/* a cursor right behind a row its text now fills waits there to
		 * wrap, as after the character that filled it */
		vt->c.wrapnext = pend[0];
		vt->saved_pri.wrapnext = pend[1];
	}
	if (!vt->pri_saved)
		vt->saved_pri.wrapnext = wn[1]; /* as the reset left it */
	vt->view = 0;
	vt_mark_all(vt);
	return true;
}

/* ---- printing --------------------------------------------------------- */

/* Format characters of width 0 that are not marks on the one before: those
 * xterm 407 counts as controls (charclass.c), but FFF9 to FFFB, of width 1 to
 * glibc; other ones of width 0 are marks to it. Out of put()'s way: inlined,
 * it made UTF-8 text 8 % slower. */
__attribute__((noinline, cold)) static bool format_char(uint32_t cp)
{
	return (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x202e) ||
	       (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff;
}

/* true when REP may still repeat what it had: a character was printed, or a
 * combining mark went on it; false for what ends it, as xterm 407 has it */
static bool put(struct vt *vt, uint32_t cp)
{
	struct vt_cursor *c = &vt->c;
	if (cp >= 0x80 && cp < 0xa0)
		return false; /* C1 controls are not supported in UTF-8 mode */
	uint32_t orig = cp; /* for REP, which goes through DEC graphics again */
	uint16_t gfx = 0;
	if (c->charset[c->gl] && cp >= 0x5f && cp <= 0x7e) {
		cp = dec_graphics[cp - 0x5f];
		gfx = ATTR_DECGFX;
	}
	int w = charwidth(cp);
	if (w == 0) /* combining characters are dropped */
		return !format_char(cp);
	if (w > vt->cols)
		return false;

	bool wrap = vt->mode & MODE_AUTOWRAP;
	if (wrap && (c->wrapnext || c->x + w > vt->cols)) {
		/* A wide glyph that did not fit leaves the last cell out of the line
		 * where that is blank: no text, so a copy, a URL and a reflow pass
		 * over it. With anything else there the cell is the line's. */
		struct row *l = vt->line[c->y];
		bool gap = !c->wrapnext && is_tail_blank(l->c[vt->cols - 1], l->tail);
		/* Not where the cursor stays in its row, the last of the screen
		 * below a scroll region: the character goes over the row's first
		 * cell, and the row goes on nowhere (J1). */
		if (c->y == vt->bot || c->y < vt->rows - 1) {
			l->wrap = gap ? vt->cols - 1 : vt->cols;
			touch(l, l->wrap);
			/* the cells before a join are content, blanks too: a row
			 * that had none has some now (J9) */
			row_changed(vt, c->y);
		}
		c->x = 0;
		index_down(vt, true);
	} else if (c->x + w > vt->cols) {
		c->x = vt->cols - w;
	}
	c->wrapnext = false;

	struct row *row = vt->line[c->y];
	if (vt->mode & MODE_INSERT)
		shift_right(vt, row, c->x, w);
	else
		fix_wide(vt, row, c->x, c->x + w - 1);
	struct cell *l = row->c;
	uint16_t base = (c->tmpl.attr & ~(ATTR_WIDE | ATTR_WDUMMY)) | gfx | ATTR_TEXT;
	l[c->x] = c->tmpl;
	l[c->x].cp = cp;
	l[c->x].attr = base | (w == 2 ? ATTR_WIDE : 0);
	if (w == 2) {
		l[c->x + 1] = l[c->x];
		l[c->x + 1].cp = 0;
		l[c->x + 1].attr = base | ATTR_WDUMMY;
	}
	wrote(row, c->x, c->x + w);
	row_changed(vt, c->y);
	mark_row(vt, c->y);
	vt->lastcp = orig;
	advance(vt, c->x + w, wrap);
	return true;
}

/* REP: n copies of cp, a character w cells wide, a row at a time, as n calls of
 * put() would leave them; in insert mode each row moves once rather than n
 * times. Without autowrap the cursor stops at the edge, and what comes after
 * writes the same cell again: nothing more to do. */
static void rep_row(struct vt *vt, uint32_t cp, int w, int n)
{
	struct vt_cursor *c = &vt->c;
	while (n > 0) {
		int x0 = c->x, y0 = c->y;
		put(vt, cp); /* wraps, DEC graphics, the cells to copy */
		n--;
		if (!(vt->mode & MODE_AUTOWRAP) && c->x == x0 && c->y == y0)
			return;
		/* only once put() went w cells on in the row: then the copy is at
		 * x - w; not after a wrap */
		if (!n || c->wrapnext || c->y != y0 || c->x != x0 + w)
			continue;
		struct row *l = vt->line[c->y];
		int x = c->x, k = MIN(n, (vt->cols - x) / w);
		if (!k)
			continue;
		struct cell cell[2] = { l->c[x - w], l->c[x - 1] }; /* put() mended the edges */
		if (vt->mode & MODE_INSERT)
			shift_right(vt, l, x, k * w); /* put() left no half glyph at x */
		else
			fix_wide(vt, l, x, x + k * w - 1); /* as put_ascii */
		for (int i = 0; i < k * w; i++)
			l->c[x + i] = cell[i % w == 0 ? 0 : 1];
		/* no row_changed(): put() was in this row, one cell before; what
		 * a run can end, it has ended */
		wrote(l, x, x + k * w);
		n -= k;
		advance(vt, c->x + k * w, vt->mode & MODE_AUTOWRAP);
	}
}

static void control(struct vt *vt, uint32_t cp)
{
	switch (cp) {
	case 0x07:
		if (vt->cb.bell)
			vt->cb.bell(vt->cb.ud);
		break;
	case 0x08:
		if (vt->c.x > 0)
			vt->c.x--;
		vt->c.wrapnext = false;
		break;
	case 0x09:
		tab(vt, 1);
		break;
	case 0x0a: case 0x0b: case 0x0c:
		index_down(vt, false);
		vt->c.wrapnext = false;
		break;
	case 0x0d:
		vt->c.x = 0;
		vt->c.wrapnext = false;
		break;
	case 0x0e:
		vt->c.gl = 1;
		break;
	case 0x0f:
		vt->c.gl = 0;
		break;
	}
}

/* ---- escape sequences ------------------------------------------------- */

/* DA: a VT220 with ANSI color */
#define DA1 "\033[?62;22c"

static void esc_dispatch(struct vt *vt, uint32_t f)
{
	if (vt->ninter == 0) {
		switch (f) {
		/* only what moves the cursor drops a pending wrap: ESC = and ESC >
		 * come with smkx and is2, and DECRC brings back the saved one */
		case '7': save_cursor(vt); break;
		case '8': restore_cursor(vt); break;
		case 'D': index_down(vt, false); vt->c.wrapnext = false; break;
		case 'E': vt->c.x = 0; index_down(vt, false); vt->c.wrapnext = false; break;
		case 'H': vt->tabs[vt->c.x] = true; break;
		case 'M': reverse_index(vt); vt->c.wrapnext = false; break;
		case 'c': vt_reset(vt); break;
		case '=': vt->mode |= MODE_APPKEYPAD; break;
		case '>': vt->mode &= ~MODE_APPKEYPAD; break;
		}
		return;
	}
	/* one intermediate and no more: ESC ( % 0 is DEC Turkish, not the graphics */
	if (vt->ninter != 1)
		return;
	switch (vt->inter[0]) {
	case '(':
		vt->c.charset[0] = f == '0';
		break;
	case ')':
		vt->c.charset[1] = f == '0';
		break;
	}
}

static int arg(const struct vt *vt, int i, int def)
{
	return i < vt->nparams && vt->params[i] ? vt->params[i] : def;
}

static uint32_t sgr_color(const struct vt *vt, int *ip, bool *ok)
{
	int i = *ip, n = vt->nparams;
	const int *p = vt->params;
	uint32_t col = 0;
	*ok = false;
	if (i + 1 < n && vt->subparam[i + 1]) {
		/* colon form: 38:5:idx, 38:2:r:g:b or 38:2:cs:r:g:b */
		int j = i + 1, end = j;
		while (end < n && vt->subparam[end])
			end++;
		int cnt = end - j;
		if (p[j] == 5 && cnt >= 2) {
			col = p[j + 1] & 0xff;
			*ok = true;
		} else if (p[j] == 2 && cnt >= 4) {
			int o = cnt >= 5 ? j + 2 : j + 1;
			col = COL_RGB | (p[o] & 0xff) << 16 | (p[o + 1] & 0xff) << 8 |
			      (p[o + 2] & 0xff);
			*ok = true;
		}
		*ip = end - 1;
	} else if (i + 2 < n && p[i + 1] == 5) {
		col = p[i + 2] & 0xff;
		*ok = true;
		*ip = i + 2;
	} else if (i + 4 < n && p[i + 1] == 2) {
		col = COL_RGB | (p[i + 2] & 0xff) << 16 | (p[i + 3] & 0xff) << 8 |
		      (p[i + 4] & 0xff);
		*ok = true;
		*ip = i + 4;
	} else {
		*ip = n; /* malformed: ignore the rest */
	}
	return col;
}

static void sgr(struct vt *vt)
{
	struct cell *t = &vt->c.tmpl;
	if (vt->nparams == 0) {
		t->attr = 0;
		t->fg = COL_DEFAULT_FG;
		t->bg = COL_DEFAULT_BG;
		return;
	}
	for (int i = 0; i < vt->nparams; i++) {
		int p = vt->params[i];
		bool ok;
		uint32_t col;
		switch (p) {
		case 0:
			t->attr = 0;
			t->fg = COL_DEFAULT_FG;
			t->bg = COL_DEFAULT_BG;
			break;
		case 1: t->attr |= ATTR_BOLD; break;
		case 2: t->attr |= ATTR_DIM; break;
		case 3: t->attr |= ATTR_ITALIC; break;
		case 4:
			if (i + 1 < vt->nparams && vt->subparam[i + 1]) {
				if (vt->params[++i])
					t->attr |= ATTR_UNDERLINE;
				else
					t->attr &= ~ATTR_UNDERLINE;
			} else {
				t->attr |= ATTR_UNDERLINE;
			}
			break;
		case 7: t->attr |= ATTR_REVERSE; break;
		case 8: t->attr |= ATTR_INVISIBLE; break;
		case 9: t->attr |= ATTR_STRIKE; break;
		case 21: t->attr |= ATTR_UNDERLINE; break;
		case 22: t->attr &= ~(ATTR_BOLD | ATTR_DIM); break;
		case 23: t->attr &= ~ATTR_ITALIC; break;
		case 24: t->attr &= ~ATTR_UNDERLINE; break;
		case 27: t->attr &= ~ATTR_REVERSE; break;
		case 28: t->attr &= ~ATTR_INVISIBLE; break;
		case 29: t->attr &= ~ATTR_STRIKE; break;
		case 38:
			col = sgr_color(vt, &i, &ok);
			if (ok)
				t->fg = col;
			continue;
		case 48:
			col = sgr_color(vt, &i, &ok);
			if (ok)
				t->bg = col;
			continue;
		case 58: /* underline color: parsed, not rendered */
			sgr_color(vt, &i, &ok);
			continue;
		case 39: t->fg = COL_DEFAULT_FG; break;
		case 49: t->bg = COL_DEFAULT_BG; break;
		default:
			if (p >= 30 && p <= 37)
				t->fg = p - 30;
			else if (p >= 40 && p <= 47)
				t->bg = p - 40;
			else if (p >= 90 && p <= 97)
				t->fg = p - 90 + 8;
			else if (p >= 100 && p <= 107)
				t->bg = p - 100 + 8;
			break;
		}
		while (i + 1 < vt->nparams && vt->subparam[i + 1])
			i++; /* skip unknown sub-parameters */
	}
}

/* The modes that are one bit of vt->mode, for setting and for DECRQM alike */
static const struct { int mode; unsigned bit; } private_bits[] = {
	{ 1, MODE_APPCURSOR }, { 7, MODE_AUTOWRAP }, { 66, MODE_APPKEYPAD },
	{ 1000, MODE_MOUSE_BTN }, { 1002, MODE_MOUSE_DRAG }, { 1003, MODE_MOUSE_ANY },
	{ 1004, MODE_FOCUS }, { 1006, MODE_MOUSE_SGR }, { 2004, MODE_BRACKETED },
};

static unsigned private_bit(int m)
{
	for (size_t i = 0; i < sizeof private_bits / sizeof *private_bits; i++)
		if (private_bits[i].mode == m)
			return private_bits[i].bit;
	return 0;
}

/* ANSI modes: IRM (4) alone; there is no newline mode (LNM, 20), CAVEATS.md */
static unsigned ansi_bit(int m)
{
	return m == 4 ? MODE_INSERT : 0;
}

static void set_private_mode(struct vt *vt, int m, bool on)
{
	unsigned bit = 0;
	switch (m) {
	case 6:
		vt->c.origin = on;
		moveto_abs(vt, 0, 0);
		return;
	case 25:
		if (on)
			vt->mode &= ~MODE_HIDECURSOR;
		else
			vt->mode |= MODE_HIDECURSOR;
		return;
	case 47:
		set_altscreen(vt, on, false);
		return;
	case 1047: /* as xterm: the alternate screen is cleared on leaving it */
		if (!on && (vt->mode & MODE_ALTSCREEN))
			for (int r = 0; r < vt->rows; r++)
				fill_row(vt->alt[r], 0, vt->cols, blank(vt), vt->cols);
		set_altscreen(vt, on, false);
		return;
	case 1048:
		if (on)
			save_cursor(vt);
		else
			restore_cursor(vt);
		return;
	case 1049:
		if (on) {
			save_cursor(vt);
			set_altscreen(vt, true, true);
		} else {
			set_altscreen(vt, false, false);
			restore_cursor(vt);
		}
		return;
	default:
		/* 66 is DECNKM, the same as ESC = and ESC > */
		bit = private_bit(m);
		if (bit & MODE_MOUSE) /* one mouse mode at a time */
			vt->mode &= ~MODE_MOUSE;
		break;
	}
	if (on)
		vt->mode |= bit;
	else
		vt->mode &= ~bit;
}

/* DECRQM answer for a private mode: 1 set, 2 reset, 3 permanently set, 4
 * permanently reset, 0 unknown. 4 is for the DEC and xterm modes mwt knows of but
 * does not have, so setting them never takes (blink, 132 columns, reverse screen,
 * margins, other mouse encodings...). 3 is for 1035, xterm's "ignore the keypad
 * mode with NumLock": mwt's keypad always sends what it sends without that mode,
 * see CAVEATS.md. 2027 stays unknown until grapheme clusters are in: see TODO. */
static int private_mode_state(const struct vt *vt, int m)
{
	unsigned bit;
	switch (m) {
	case 3: case 4: case 5: case 9: case 12: case 18: case 19: case 38: case 40:
	case 41: case 42: case 45: case 60: case 67: case 69: case 95: case 1001:
	case 1005: case 1015: case 1045:
		return 4;
	case 1035:
		return 3;
	case 6: return vt->c.origin ? 1 : 2;
	case 25: return vt->mode & MODE_HIDECURSOR ? 2 : 1;
	case 47: case 1047: case 1049: bit = MODE_ALTSCREEN; break;
	default:
		bit = private_bit(m);
		if (!bit)
			return 0;
	}
	return vt->mode & bit ? 1 : 2;
}

/* What a cell adds to the DECRQCRA checksum, as xterm counts it: the character
 * as a DEC 8-bit code (anything beyond Latin-1, wide halves included, as ESC;
 * Latin-1 cut to 7 bits; DEC graphics as the code it came in as) plus a bit per
 * video attribute, but for blink, which mwt does not keep. Every cell counts, an
 * erased one as a blank: that is what esctest expects of xterm by default. Current
 * xterm skips cells never written to unless checksumExtension says otherwise, and
 * mwt does not tell them apart. */
static int checksum_cell(const struct cell *c)
{
	uint32_t cp = c->cp;
	int ch = 0x1b;
	if (c->attr & ATTR_DECGFX) {
		for (int i = 0; i < 32; i++)
			if (dec_graphics[i] == cp)
				ch = 0x5f + i;
	} else if (cp == 0x7f || cp == 0xff) {
		ch = 0;
	} else if (cp >= 0x20 && cp <= 0xff && cp != 0xa0) {
		ch = (int)(cp & 0x7f);
	}
	if (c->attr & ATTR_INVISIBLE)
		ch += 0x08;
	if (c->attr & ATTR_UNDERLINE)
		ch += 0x10;
	if (c->attr & ATTR_REVERSE)
		ch += 0x20;
	if (c->attr & ATTR_BOLD)
		ch += 0x80;
	return ch;
}

/* A rectangle edge as xterm limits it: in origin mode relative to the scroll
 * region and kept inside it, otherwise inside the screen. 1-based. */
static int rect_edge(const struct vt *vt, int v, bool row)
{
	bool org = vt->c.origin && row;
	int lo = org ? vt->top + 1 : 1;
	int hi = row ? (org ? vt->bot + 1 : vt->rows) : vt->cols;
	if (org)
		v += vt->top;
	return CLAMP(v, lo, hi);
}

/* DECRQCRA: CSI Pi ; Pg ; Pt ; Pl ; Pb ; Pr * y, answered with DCS Pi ! ~ xxxx ST.
 * Off unless the terminal allows it, like xterm, where GetChecksum is among the
 * disallowed window operations: it lets any program read the screen. The page is
 * ignored, and with fewer than three parameters xterm takes the rectangle from
 * the first ones, defaults included in origin mode; this follows it. */
static void decrqcra(struct vt *vt)
{
	if (!vt->allow_checksum)
		return;
	int off = vt->nparams > 2 ? 2 : 0, n = vt->nparams - off;
	bool org = vt->c.origin;
	int dflt[4] = { org ? vt->top + 1 : 1, 1, org ? vt->bot + 1 : vt->rows, vt->cols };
	int e[4];
	for (int i = 0; i < 4; i++) {
		int v = i < n && vt->params[off + i] > 0 ? vt->params[off + i] : dflt[i];
		e[i] = rect_edge(vt, v, i % 2 == 0);
	}
	unsigned sum = 0; /* 4096x2048 cells of up to 374 each pass INT_MAX */
	if (e[0] <= e[2] && e[1] <= e[3])
		for (int r = e[0] - 1; r < e[2]; r++)
			for (int x = e[1] - 1; x < e[3]; x++)
				sum += checksum_cell(&vt->line[r]->c[x]);
	reply(vt, "\033P%d!~%04X\033\\", arg(vt, 0, 0), -sum & 0xffff);
}

static void csi_dispatch(struct vt *vt, uint32_t f)
{
	struct vt_cursor *c = &vt->c;
	int n = arg(vt, 0, 1);
	uint32_t rep_cp = vt->lastcp; /* for REP; feed() forgets it after this */

	/* sub-parameters are SGR's alone: CSI 1:5 H is no CUP to row 1, column 5 */
	if (vt->colon && (f != 'm' || vt->priv || vt->ninter))
		return;
	if (vt->priv == '?') {
		if (vt->ninter == 1 && vt->inter[0] == '$' && f == 'p') { /* DECRQM */
			int m = arg(vt, 0, 0);
			reply(vt, "\033[?%d;%d$y", m, private_mode_state(vt, m));
		} else if (vt->ninter) {
			/* nothing else with intermediates */
		} else if (f == 'h' || f == 'l')
			for (int i = 0; i < vt->nparams; i++)
				set_private_mode(vt, vt->params[i], f == 'h');
		else if (f == 'n' && arg(vt, 0, 0) == 6)
			reply(vt, "\033[?%d;%dR", c->y + 1 - origin_row(vt), c->x + 1);
		return;
	}
	if (vt->priv == '>') {
		/* as xterm, only for 0 or none: CSI > 1 c and CSI > 1 q are no
		 * requests of these */
		if (vt->ninter || arg(vt, 0, 0) != 0)
			return;
		if (f == 'c')
			reply(vt, "\033[>1;10;0c");
		else if (f == 'q')
			reply(vt, "\033P>|mwt(" MWT_VERSION ")\033\\");
		return;
	}
	if (vt->priv)
		return;
	if (vt->ninter) {
		if (vt->ninter != 1) /* none here has two; see collect() */
			return;
		if (vt->inter[0] == ' ' && f == 'q') { /* DECSCUSR */
			int s = arg(vt, 0, 0);
			if (s > 6) /* xterm keeps the style */
				return;
			vt->cursor_style = s <= 2 ? CURSOR_BLOCK :
					   s <= 4 ? CURSOR_UNDERLINE : CURSOR_BAR;
			mark_row(vt, c->y);
		} else if (vt->inter[0] == '!' && f == 'p') {
			soft_reset(vt);
		} else if (vt->inter[0] == '*' && f == 'y') {
			decrqcra(vt);
		} else if (vt->inter[0] == '$' && f == 'p') { /* DECRQM, ANSI modes */
			int m = arg(vt, 0, 0);
			unsigned bit = ansi_bit(m);
			/* the ECMA-48 modes of a terminal mwt is not (guarded areas,
			 * a locked keyboard, editing extents...) are permanently reset,
			 * and so is LNM (20), which mwt does not have; SRM (12) stays
			 * unknown, terminals disagree on what reset means */
			bool never = (m >= 1 && m <= 20 && m != 4 && m != 6 && m != 8 &&
				      m != 9 && m != 12);
			reply(vt, "\033[%d;%d$y", m, bit ? (vt->mode & bit ? 1 : 2) : never ? 4 : 0);
		}
		return;
	}

	struct row *l = vt->line[c->y];
	switch (f) {
	case '@': /* ICH */
		n = MIN(n, vt->cols - c->x);
		shift_right(vt, l, c->x, n);
		fill_row(l, c->x, c->x + n, blank(vt), vt->cols);
		row_changed(vt, c->y);
		mark_row(vt, c->y);
		c->wrapnext = false;
		break;
	case 'A': moveto(vt, c->x, MAX(c->y - n, c->y >= vt->top ? vt->top : 0)); break;
	case 'B': case 'e':
		moveto(vt, c->x, MIN(c->y + n, c->y <= vt->bot ? vt->bot : vt->rows - 1));
		break;
	case 'C': case 'a': moveto(vt, c->x + n, c->y); break;
	case 'D': moveto(vt, c->x - n, c->y); break;
	case 'E': moveto(vt, 0, MIN(c->y + n, c->y <= vt->bot ? vt->bot : vt->rows - 1)); break;
	case 'F': moveto(vt, 0, MAX(c->y - n, c->y >= vt->top ? vt->top : 0)); break;
	case 'G': case '`': moveto(vt, n - 1, c->y); break;
	case 'H': case 'f': moveto_abs(vt, arg(vt, 1, 1) - 1, n - 1); break;
	case 'I': tab(vt, n); break;
	case 'J': /* ED */
		switch (arg(vt, 0, 0)) {
		/* as xterm: erasing the screen drops a pending wrap, ED 3 does not */
		case 0:
			if (c->y == 0 && c->x == 0)
				hist_unwrap(vt);
			clear_line(vt, c->y, c->x, vt->cols - 1);
			for (int r = c->y + 1; r < vt->rows; r++)
				clear_line(vt, r, 0, vt->cols - 1);
			c->wrapnext = false;
			break;
		case 1:
			hist_unwrap(vt);
			for (int r = 0; r < c->y; r++)
				clear_line(vt, r, 0, vt->cols - 1);
			clear_line(vt, c->y, 0, c->x);
			c->wrapnext = false;
			break;
		case 2:
			hist_unwrap(vt);
			for (int r = 0; r < vt->rows; r++)
				clear_line(vt, r, 0, vt->cols - 1);
			c->wrapnext = false;
			break;
		case 3:
			vt->hist_count = 0;
			vt->view = 0;
			vt_mark_all(vt);
			break;
		}
		break;
	case 'K': /* EL */
		switch (arg(vt, 0, 0)) {
		case 0: clear_line(vt, c->y, c->x, vt->cols - 1); break;
		case 1: clear_line(vt, c->y, 0, c->x); break;
		case 2: clear_line(vt, c->y, 0, vt->cols - 1); break;
		}
		c->wrapnext = false; /* as xterm: nothing left to wrap onto */
		break;
	case 'L':
		if (c->y >= vt->top && c->y <= vt->bot) {
			scroll_down(vt, c->y, n);
			c->x = 0;
			c->wrapnext = false;
		}
		break;
	case 'M':
		if (c->y >= vt->top && c->y <= vt->bot) {
			scroll_up(vt, c->y, n, false);
			c->x = 0;
			c->wrapnext = false;
		}
		break;
	case 'P': /* DCH */
		n = MIN(n, vt->cols - c->x);
		fix_wide(vt, l, c->x, c->x + n - 1);
		/* cells before the wrap go and blanks come in at the end: the line no
		 * longer goes on, also where a window grown wider left the wrap in
		 * the middle of the row, on the alternate screen; moved left it would
		 * look like one of those */
		if (c->x < l->wrap)
			l->wrap = 0;
		memmove(&l->c[c->x], &l->c[c->x + n], (size_t)(vt->cols - c->x - n) * sizeof *l->c);
		fill_row(l, vt->cols - n, vt->cols, blank(vt), vt->cols);
		row_changed(vt, c->y);
		mark_row(vt, c->y);
		c->wrapnext = false;
		break;
	case 'S': scroll_up(vt, vt->top, n, false); break;
	case 'T': scroll_down(vt, vt->top, n); break;
	case 'X':
		clear_line(vt, c->y, c->x, c->x + n - 1);
		c->wrapnext = false;
		break;
	case 'Z': tab(vt, -n); break;
	case 'b': /* REP */
	{
		/* Past a screenful and a row more, more only adds the same lines to
		 * the history: the count is cut to that, keeping what it leaves in
		 * the last row, so 8 bytes cannot ask for 65535 rows of work. */
		/* what has no width to wcwidth() is printed once but not repeated, as
		 * xterm; DEC graphics are all one cell, as what they stand for */
		int w = !rep_cp ? 0 : rep_cp < 0x300 ? 1 : wcwidth((wchar_t)rep_cp);
		if (w < 1 || w > vt->cols)
			break;
		int per_row = vt->cols / w, limit = vt->cols * vt->rows + per_row;
		if (n > limit)
			n = limit + (n - limit) % per_row;
		rep_row(vt, rep_cp, w, n);
		break;
	}
	case 'c':
		if (arg(vt, 0, 0) == 0)
			reply(vt, DA1);
		break;
	case 'd': moveto_abs(vt, c->x, n - 1); break;
	case 'g':
		if (arg(vt, 0, 0) == 0)
			vt->tabs[c->x] = false;
		else if (arg(vt, 0, 0) == 3)
			memset(vt->tabs, 0, vt->cols * sizeof *vt->tabs);
		break;
	case 'h': case 'l':
		for (int i = 0; i < vt->nparams; i++) {
			unsigned bit = ansi_bit(vt->params[i]);
			if (f == 'h')
				vt->mode |= bit;
			else
				vt->mode &= ~bit;
		}
		break;
	case 'm': sgr(vt); break;
	case 'n':
		if (arg(vt, 0, 0) == 5)
			reply(vt, "\033[0n");
		else if (arg(vt, 0, 0) == 6)
			reply(vt, "\033[%d;%dR", c->y + 1 - origin_row(vt), c->x + 1);
		break;
	case 'r': { /* DECSTBM */
		int t = arg(vt, 0, 1) - 1, b = arg(vt, 1, vt->rows) - 1;
		b = MIN(b, vt->rows - 1);
		if (t < b) {
			vt->top = t;
			vt->bot = b;
			moveto_abs(vt, 0, 0);
		}
		break;
	}
	case 's': save_cursor(vt); break;
	case 'u': restore_cursor(vt); break;
	case 't':
		if (arg(vt, 0, 0) == 18)
			reply(vt, "\033[8;%d;%dt", vt->rows, vt->cols);
		break;
	}
}

/* One color for an OSC query, in the form xterm answers with: 16 bits per
 * channel, the byte doubled. which is "4;N" for the palette, or "10".."12". */
static void color_reply(struct vt *vt, const char *which, uint32_t c)
{
	uint32_t v = vt->cb.color(vt->cb.ud, c);
	unsigned r = v >> 16 & 0xff, g = v >> 8 & 0xff, b = v & 0xff;
	reply(vt, "\033]%s;rgb:%02x%02x/%02x%02x/%02x%02x%s", which, r, r, g, g, b, b,
	      vt->osc_bel ? "\a" : "\033\\");
}

/* Strict decimal, for OSC numbers: atoi takes "4x" as 4. */
static bool osc_number(const char *s, int max, int *out)
{
	int v = 0;
	if (!*s)
		return false;
	for (; *s; s++) {
		if (*s < '0' || *s > '9' || (v = v * 10 + (*s - '0')) > max)
			return false;
	}
	*out = v;
	return true;
}

/* The next ';'-separated field of *s, empty ones included; NULL at the end. */
static char *osc_field(char **s)
{
	char *f = *s;
	if (!f)
		return NULL;
	char *semi = strchr(f, ';');
	*s = semi ? semi + 1 : NULL;
	if (semi)
		*semi = 0;
	return f;
}

/* Colors are only answered, not changed: they come from config.h and the
 * mwtrc, a program setting one would not get it. OSC 4 takes pairs "N;spec"; OSC 10 to 12
 * take a list in which each further entry is the next number, as in xterm, so
 * "10;?;?" asks for the foreground and the background. */
static void osc_colors(struct vt *vt, int num, char *args)
{
	if (!vt->cb.color)
		return;
	if (num == 4) {
		for (;;) {
			char *n = osc_field(&args);
			char *spec = osc_field(&args);
			int idx;
			if (!n || !spec)
				return;
			if (strcmp(spec, "?") || !osc_number(n, 255, &idx))
				continue;
			char which[16];
			snprintf(which, sizeof which, "4;%d", idx);
			color_reply(vt, which, (uint32_t)idx);
		}
	}
	static const uint32_t dyn[3] = { COL_DEFAULT_FG, COL_DEFAULT_BG, COL_CURSOR };
	for (int i = num; i <= 12; i++) {
		char *spec = osc_field(&args);
		if (!spec)
			return;
		if (!strcmp(spec, "?")) {
			char which[16];
			snprintf(which, sizeof which, "%d", i);
			color_reply(vt, which, dyn[i - 10]);
		}
	}
}

static void osc_dispatch(struct vt *vt)
{
	vt->osc[vt->osc_len] = 0;
	char *semi = strchr(vt->osc, ';');
	if (!semi)
		return;
	*semi = 0;
	int num;
	if (!osc_number(vt->osc, 999, &num))
		return;
	/* a title that did not fit is cut; a query is not answered in part */
	if ((num == 0 || num == 2) && vt->cb.title)
		vt->cb.title(vt->cb.ud, semi + 1);
	else if (vt->osc_full)
		return;
	else if (num == 4 || (num >= 10 && num <= 12))
		osc_colors(vt, num, semi + 1);
}

/* ---- the state machine ------------------------------------------------ */

static void clear_seq(struct vt *vt)
{
	vt->nparams = 0;
	vt->params_full = false;
	vt->colon = false;
	vt->priv = 0;
	vt->ninter = 0;
}

/* A third intermediate is not kept, and need not be: no sequence here has more
 * than one, so the dispatchers drop what comes with two. */
static void collect(struct vt *vt, uint32_t cp)
{
	if (vt->ninter < 2)
		vt->inter[vt->ninter++] = (char)cp;
}

static void param(struct vt *vt, uint32_t cp)
{
	if (vt->nparams == 0) {
		vt->nparams = 1;
		vt->params[0] = 0;
		vt->subparam[0] = false;
	}
	if (cp == ':') /* also past the last slot */
		vt->colon = true;
	if (vt->params_full)
		return;
	if (cp == ';' || cp == ':') {
		if (vt->nparams < VT_MAX_PARAMS) {
			vt->params[vt->nparams] = 0;
			vt->subparam[vt->nparams] = cp == ':';
			vt->nparams++;
		} else {
			vt->params_full = true;
		}
	} else {
		int *p = &vt->params[vt->nparams - 1];
		*p = MIN(*p * 10 + (int)(cp - '0'), 65535);
	}
}

static void osc_put(struct vt *vt, uint32_t cp)
{
	char buf[4];
	int n;
	if (cp < 0x80) {
		buf[0] = cp; n = 1;
	} else if (cp < 0x800) {
		buf[0] = 0xc0 | cp >> 6; buf[1] = 0x80 | (cp & 0x3f); n = 2;
	} else if (cp < 0x10000) {
		buf[0] = 0xe0 | cp >> 12; buf[1] = 0x80 | (cp >> 6 & 0x3f);
		buf[2] = 0x80 | (cp & 0x3f); n = 3;
	} else {
		buf[0] = 0xf0 | cp >> 18; buf[1] = 0x80 | (cp >> 12 & 0x3f);
		buf[2] = 0x80 | (cp >> 6 & 0x3f); buf[3] = 0x80 | (cp & 0x3f); n = 4;
	}
	/* cut for good at the first one that does not fit: a shorter one after it
	 * would leave a title with a hole */
	if (vt->osc_full || vt->osc_len + n >= VT_OSC_MAX) {
		vt->osc_full = true;
		return;
	}
	memcpy(vt->osc + vt->osc_len, buf, n);
	vt->osc_len += n;
}

static void feed(struct vt *vt, uint32_t cp)
{
	/* "anywhere" transitions */
	if (cp == 0x18 || cp == 0x1a) {
		vt->state = ST_GROUND;
		vt->lastcp = 0;
		return;
	}
	if (cp == 0x1b) {
		if (vt->state == ST_OSC) {
			vt->osc_bel = false;
			osc_dispatch(vt);
		}
		vt->state = ST_ESC;
		clear_seq(vt);
		return;
	}

	/* REP repeats what was printed just before: a code point that ends in
	 * the ground state without being printed ends that, as in xterm 407; a
	 * control within a sequence, or one cut off by ESC, does not */
	bool keep = false;
	switch (vt->state) {
	case ST_GROUND:
		if (cp < 0x20 || cp == 0x7f)
			control(vt, cp);
		else
			keep = put(vt, cp);
		break;
	case ST_ESC:
		if (cp < 0x20)
			control(vt, cp);
		else if (cp < 0x30) {
			collect(vt, cp);
			vt->state = ST_ESC_INTER;
		} else if (cp == '[') {
			vt->state = ST_CSI_ENTRY;
		} else if (cp == ']') {
			vt->osc_len = 0;
			vt->osc_full = false;
			vt->state = ST_OSC;
		} else if (cp == 'P' || cp == 'X' || cp == '^' || cp == '_') {
			vt->state = ST_STR_IGNORE; /* DCS, SOS, PM, APC */
		} else if (cp < 0x7f) {
			esc_dispatch(vt, cp);
			vt->state = ST_GROUND;
		} else if (cp != 0x7f) { /* DEL is ignored, as xterm */
			vt->state = ST_GROUND;
		}
		break;
	case ST_ESC_INTER:
		if (cp < 0x20)
			control(vt, cp);
		else if (cp < 0x30)
			collect(vt, cp);
		else if (cp < 0x7f) {
			esc_dispatch(vt, cp);
			vt->state = ST_GROUND;
		} else if (cp != 0x7f) { /* not ASCII: ends it, as in ST_ESC */
			vt->state = ST_GROUND;
		}
		break;
	case ST_CSI_ENTRY:
	case ST_CSI_PARAM:
		if (cp < 0x20)
			control(vt, cp);
		else if ((cp >= '0' && cp <= '9') || cp == ';' || cp == ':') {
			param(vt, cp);
			vt->state = ST_CSI_PARAM;
		} else if (cp >= 0x3c && cp <= 0x3f) {
			if (vt->state == ST_CSI_ENTRY) {
				vt->priv = (char)cp;
				vt->state = ST_CSI_PARAM;
			} else {
				vt->state = ST_CSI_IGNORE;
			}
		} else if (cp >= 0x20 && cp < 0x30) {
			collect(vt, cp);
			vt->state = ST_CSI_INTER;
		} else if (cp >= 0x40 && cp < 0x7f) {
			csi_dispatch(vt, cp);
			vt->state = ST_GROUND;
		} else if (cp >= 0x80) {
			vt->state = ST_GROUND;
		}
		break;
	case ST_CSI_INTER:
		if (cp < 0x20)
			control(vt, cp);
		else if (cp < 0x30)
			collect(vt, cp);
		else if (cp < 0x40)
			vt->state = ST_CSI_IGNORE;
		else if (cp < 0x7f) {
			csi_dispatch(vt, cp);
			vt->state = ST_GROUND;
		} else if (cp != 0x7f) {
			vt->state = ST_GROUND;
		}
		break;
	case ST_CSI_IGNORE:
		if (cp < 0x20)
			control(vt, cp);
		else if ((cp >= 0x40 && cp < 0x7f) || cp >= 0x80)
			vt->state = ST_GROUND;
		break;
	case ST_OSC:
		if (cp == 0x07) {
			vt->osc_bel = true;
			osc_dispatch(vt);
			vt->state = ST_GROUND;
		} else if (cp >= 0x20 && cp != 0x7f && (cp < 0x80 || cp >= 0xa0)) {
			/* titles go on to the compositor and its bars: no controls */
			osc_put(vt, cp);
		}
		break;
	case ST_STR_IGNORE:
		break;
	}
	if (vt->state == ST_GROUND && !keep)
		vt->lastcp = 0;
}

/* A run of printable ASCII in the ground state, as put() writes it one by one, up
 * to the end of the row; 0 where put() has more to do: a pending wrap, insert
 * mode. Wide glyphs need mending only at the run's edges, what lies between is
 * overwritten. About three times faster on plain text. DEC graphics too, which
 * ncurses draws its borders with: they took put() at six times the cost. */
static size_t put_ascii(struct vt *vt, const unsigned char *s, size_t n)
{
	struct vt_cursor *c = &vt->c;
	if (c->wrapnext || (vt->mode & MODE_INSERT))
		return 0;
	bool gfx = c->charset[c->gl];
	size_t room = (size_t)(vt->cols - c->x), k = 0;
	/* a graphic a locale counts wide stops the run: put() knows what to do */
	while (k < n && k < room && s[k] >= 0x20 && s[k] < 0x7f &&
	       !(gfx && s[k] >= 0x5f && charwidth(dec_graphics[s[k] - 0x5f]) != 1))
		k++;
	if (!k)
		return 0;
	struct row *row = vt->line[c->y];
	int x0 = c->x, x1 = c->x + (int)k - 1;
	fix_wide(vt, row, x0, x1);
	struct cell *l = row->c;
	struct cell tm = c->tmpl;
	tm.attr = (tm.attr & ~(ATTR_WIDE | ATTR_WDUMMY)) | ATTR_TEXT;
	if (gfx) {
		uint16_t attr = tm.attr;
		for (size_t i = 0; i < k; i++) {
			bool g = s[i] >= 0x5f;
			tm.cp = g ? dec_graphics[s[i] - 0x5f] : s[i];
			tm.attr = g ? attr | ATTR_DECGFX : attr;
			l[x0 + i] = tm;
		}
	} else {
		for (size_t i = 0; i < k; i++) {
			tm.cp = s[i];
			l[x0 + i] = tm;
		}
	}
	wrote(row, x0, x1 + 1);
	row_changed(vt, c->y);
	mark_row(vt, c->y);
	vt->lastcp = s[k - 1];
	advance(vt, x1 + 1, vt->mode & MODE_AUTOWRAP);
	return k;
}

void vt_write(struct vt *vt, const char *buf, size_t len)
{
	const unsigned char *s = (const unsigned char *)buf;
	for (size_t i = 0; i < len; i++) {
		unsigned char b = s[i];
		if (!vt->utf8_need && vt->state == ST_GROUND && b >= 0x20 && b < 0x7f) {
			size_t k = put_ascii(vt, s + i, len - i);
			if (k) {
				i += k - 1;
				continue;
			}
		}
		/* CSI parameters straight to param(), as feed() would take them:
		 * byte by byte through feed() cost cup and sgr a third */
		if (!vt->utf8_need && (vt->state == ST_CSI_ENTRY || vt->state == ST_CSI_PARAM) &&
		    ((b >= '0' && b <= '9') || b == ';' || b == ':')) {
			do
				param(vt, s[i]);
			while (++i < len && ((s[i] >= '0' && s[i] <= '9') || s[i] == ';' || s[i] == ':'));
			vt->state = ST_CSI_PARAM;
			i--;
			continue;
		}
		if (vt->utf8_need) {
			if ((b & 0xc0) == 0x80) {
				vt->utf8_cp = vt->utf8_cp << 6 | (b & 0x3f);
				if (--vt->utf8_need == 0) {
					uint32_t cp = vt->utf8_cp;
					/* an overlong form is refused too: E0 80 9B would
					 * be ESC, past a filter that drops the byte 0x1B */
					if (cp < vt->utf8_min || cp > 0x10ffff ||
					    (cp >= 0xd800 && cp < 0xe000))
						cp = 0xfffd;
					feed(vt, cp);
				}
				continue;
			}
			vt->utf8_need = 0;
			feed(vt, 0xfffd); /* truncated sequence; reprocess b */
		}
		if (b < 0x80) {
			feed(vt, b);
		} else if ((b & 0xe0) == 0xc0 && b >= 0xc2) {
			vt->utf8_cp = b & 0x1f;
			vt->utf8_min = 0x80;
			vt->utf8_need = 1;
		} else if ((b & 0xf0) == 0xe0) {
			vt->utf8_cp = b & 0x0f;
			vt->utf8_min = 0x800;
			vt->utf8_need = 2;
		} else if ((b & 0xf8) == 0xf0 && b <= 0xf4) {
			vt->utf8_cp = b & 0x07;
			vt->utf8_min = 0x10000;
			vt->utf8_need = 3;
		} else {
			feed(vt, 0xfffd);
		}
	}
}

/* ---- keys ------------------------------------------------------------- */

/* The xterm way: with modifiers, the parameter 1 + Shift + 2 Alt + 4 Ctrl goes
 * into the sequence (CSI 1 ; m A, CSI n ; m ~); without, cursor keys follow
 * DECCKM and F1 to F4 are SS3 P to S. Keys that send a byte get Alt as an ESC in
 * front instead. The keypad mode changes nothing, see CAVEATS.md. */
size_t vt_key(const struct vt *vt, enum vt_key key, unsigned mods, char *buf, size_t cap)
{
	static const char cursor[] = "BCDHFE"; /* after A for VT_KEY_UP */
	static const char fn[] = "PQRS";
	static const int tilde[] = {
		[VT_KEY_INSERT] = 2, [VT_KEY_DELETE] = 3, [VT_KEY_PAGE_UP] = 5,
		[VT_KEY_PAGE_DOWN] = 6, [VT_KEY_MENU] = 29, [VT_KEY_F5] = 15, [VT_KEY_F6] = 17,
		[VT_KEY_F7] = 18, [VT_KEY_F8] = 19, [VT_KEY_F9] = 20, [VT_KEY_F10] = 21,
		[VT_KEY_F11] = 23, [VT_KEY_F12] = 24,
	};
	bool shift = mods & VT_MOD_SHIFT, alt = mods & VT_MOD_ALT, ctrl = mods & VT_MOD_CTRL;
	int m = 1 + shift + 2 * alt + 4 * ctrl;
	const char *s = NULL;
	int n;
	char tmp[16];

	if ((unsigned)key >= VT_KEY_COUNT)
		return 0;
	if (key <= VT_KEY_BEGIN) {
		char c = key == VT_KEY_UP ? 'A' : cursor[key - VT_KEY_DOWN];
		n = m > 1 ? snprintf(tmp, sizeof tmp, "\033[1;%d%c", m, c) :
		    snprintf(tmp, sizeof tmp, (vt->mode & MODE_APPCURSOR) ? "\033O%c" : "\033[%c", c);
	} else if (key >= VT_KEY_F1 && key <= VT_KEY_F4) {
		char c = fn[key - VT_KEY_F1];
		n = m > 1 ? snprintf(tmp, sizeof tmp, "\033[1;%d%c", m, c) :
		    snprintf(tmp, sizeof tmp, "\033O%c", c);
	} else if (key < VT_KEY_BACKSPACE) {
		n = m > 1 ? snprintf(tmp, sizeof tmp, "\033[%d;%d~", tilde[key], m) :
		    snprintf(tmp, sizeof tmp, "\033[%d~", tilde[key]);
	} else {
		switch (key) {
		case VT_KEY_BACKSPACE: s = ctrl ? "\b" : "\x7f"; break;
		case VT_KEY_ENTER: s = "\r"; break;
		case VT_KEY_TAB: s = shift ? "\033[Z" : "\t"; break;
		case VT_KEY_BACKTAB: s = "\033[Z"; alt = false; break;
		case VT_KEY_ESCAPE: s = "\033"; break;
		default: return 0;
		}
		n = snprintf(tmp + 1, sizeof tmp - 1, "%s", s);
		if (alt)
			tmp[0] = '\033';
		else
			memmove(tmp, tmp + 1, (size_t)n);
		n += alt;
	}
	if (n <= 0 || (size_t)n > cap)
		return 0;
	memcpy(buf, tmp, (size_t)n);
	return (size_t)n;
}
