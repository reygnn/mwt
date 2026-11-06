/* vt.c - a small VT parser after Paul Williams' DEC state machine
 * (https://vt100.net/emu/dec_ansi_parser), fed with decoded UTF-8.
 * Implements the subset of xterm's ctlseqs that current software uses. */
#define _XOPEN_SOURCE 700
#include "vt.h"

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

static int charwidth(uint32_t cp)
{
	if (cp < 0x300)
		return 1;
	int w = wcwidth((wchar_t)cp);
	return w < 0 ? 1 : w;
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

static void free_screen(struct cell **s, int rows)
{
	if (!s)
		return;
	for (int r = 0; r < rows; r++)
		free(s[r]);
	free(s);
}

/* a blank screen, or NULL with nothing left allocated */
static struct cell **alloc_screen(int cols, int rows)
{
	struct cell **s = calloc(rows, sizeof *s);
	if (!s)
		return NULL;
	struct cell b = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
	for (int r = 0; r < rows; r++) {
		s[r] = malloc((size_t)cols * sizeof **s);
		if (!s[r]) {
			free_screen(s, rows);
			return NULL;
		}
		fill(s[r], 0, cols, b);
	}
	return s;
}

static void dirty_range(struct vt *vt, int a, int b)
{
	for (int r = MAX(a, 0); r <= b && r < vt->rows; r++)
		vt->dirty[r] = true;
}

void vt_mark_all(struct vt *vt)
{
	dirty_range(vt, 0, vt->rows - 1);
}

/* break up wide glyphs that are partially overwritten in [x0, x1] */
static void fix_wide(struct vt *vt, struct cell *l, int x0, int x1)
{
	if (x0 > 0 && (l[x0].attr & ATTR_WDUMMY))
		l[x0 - 1] = blank(vt);
	if (x1 + 1 < vt->cols && (l[x1].attr & ATTR_WIDE))
		l[x1 + 1] = blank(vt);
}

static void clear_line(struct vt *vt, int y, int x0, int x1)
{
	x0 = CLAMP(x0, 0, vt->cols - 1);
	x1 = CLAMP(x1, 0, vt->cols - 1);
	if (x0 > x1)
		return;
	fix_wide(vt, vt->line[y], x0, x1);
	fill(vt->line[y], x0, x1 + 1, blank(vt));
	vt->dirty[y] = true;
}

/* ---- scrollback ------------------------------------------------------- */

static void push_hist(struct vt *vt, const struct cell *l, int len)
{
	if (vt->hist_cap <= 0)
		return;
	/* trim trailing default blanks unless the line wraps */
	if (!(l[len - 1].attr & ATTR_WRAP))
		while (len > 0 && l[len - 1].cp == ' ' &&
		       l[len - 1].bg == COL_DEFAULT_BG && !l[len - 1].attr)
			len--;
	int slot = vt->hist_head;
	struct cell *dst = realloc(vt->hist[slot], MAX(len, 1) * sizeof *dst);
	if (!dst)
		return;
	memcpy(dst, l, len * sizeof *dst);
	vt->hist[slot] = dst;
	vt->hist_len[slot] = len;
	vt->hist_head = (slot + 1) % vt->hist_cap;
	if (vt->hist_count < vt->hist_cap)
		vt->hist_count++;
	vt->scrolled++;
	if (vt->view > 0) /* keep the viewed text in place */
		vt->view = MIN(vt->view + 1, vt->hist_count);
}

const struct cell *vt_abs_line(const struct vt *vt, int64_t abs, int *len)
{
	int64_t rel = abs - (int64_t)vt->scrolled;
	if (rel >= 0) {
		if (rel >= vt->rows)
			goto none;
		*len = vt->cols;
		return vt->line[rel];
	}
	int64_t i = -rel - 1;
	if (i >= vt->hist_count)
		goto none;
	int slot = (int)((vt->hist_head - 1 - i + vt->hist_cap) % vt->hist_cap);
	*len = vt->hist_len[slot];
	return vt->hist[slot];
none:
	*len = 0;
	return NULL;
}

const struct cell *vt_view_line(const struct vt *vt, int r, int *len)
{
	return vt_abs_line(vt, (int64_t)vt->scrolled - vt->view + r, len);
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

static void scroll_up(struct vt *vt, int orig, int n)
{
	if (orig < vt->top || orig > vt->bot)
		return;
	n = CLAMP(n, 0, vt->bot - orig + 1);
	if (!n)
		return;
	if (orig == 0 && !(vt->mode & MODE_ALTSCREEN))
		for (int i = 0; i < n; i++)
			push_hist(vt, vt->line[i], vt->cols);
	struct cell *tmp[n];
	memcpy(tmp, &vt->line[orig], n * sizeof *tmp);
	memmove(&vt->line[orig], &vt->line[orig + n],
		(vt->bot - orig + 1 - n) * sizeof *tmp);
	memcpy(&vt->line[vt->bot - n + 1], tmp, n * sizeof *tmp);
	for (int r = vt->bot - n + 1; r <= vt->bot; r++)
		fill(vt->line[r], 0, vt->cols, blank(vt));
	dirty_range(vt, orig, vt->bot);
}

static void scroll_down(struct vt *vt, int orig, int n)
{
	if (orig < vt->top || orig > vt->bot)
		return;
	n = CLAMP(n, 0, vt->bot - orig + 1);
	if (!n)
		return;
	struct cell *tmp[n];
	memcpy(tmp, &vt->line[vt->bot - n + 1], n * sizeof *tmp);
	memmove(&vt->line[orig + n], &vt->line[orig],
		(vt->bot - orig + 1 - n) * sizeof *tmp);
	memcpy(&vt->line[orig], tmp, n * sizeof *tmp);
	for (int r = orig; r < orig + n; r++)
		fill(vt->line[r], 0, vt->cols, blank(vt));
	dirty_range(vt, orig, vt->bot);
}

static void index_down(struct vt *vt)
{
	if (vt->c.y == vt->bot)
		scroll_up(vt, vt->top, 1);
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

/* absolute position, honoring origin mode */
static void moveto_abs(struct vt *vt, int x, int y)
{
	moveto(vt, x, y + (vt->c.origin ? vt->top : 0));
}

static void save_cursor(struct vt *vt)
{
	if (vt->mode & MODE_ALTSCREEN)
		vt->saved_alt = vt->c;
	else
		vt->saved_pri = vt->c;
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
	if (n > 0)
		while (n-- && x < vt->cols - 1)
			while (++x < vt->cols - 1 && !vt->tabs[x])
				;
	else
		while (n++ && x > 0)
			while (--x > 0 && !vt->tabs[x])
				;
	vt->c.x = x;
	vt->c.wrapnext = false;
}

/* ---- screens and reset ------------------------------------------------ */

static void set_altscreen(struct vt *vt, bool on, bool clear)
{
	if (!!(vt->mode & MODE_ALTSCREEN) == on)
		return;
	if (on) {
		vt->mode |= MODE_ALTSCREEN;
		vt->line = vt->alt;
		if (clear)
			for (int r = 0; r < vt->rows; r++)
				fill(vt->alt[r], 0, vt->cols, blank(vt));
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
	vt->mode |= MODE_AUTOWRAP;
	vt->top = 0;
	vt->bot = vt->rows - 1;
	int x = vt->c.x, y = vt->c.y;
	default_cursor(&vt->c);
	vt->c.x = x;
	vt->c.y = y;
	vt->saved_pri = vt->saved_alt = vt->c;
	vt->saved_pri.x = vt->saved_pri.y = 0;
	vt->saved_alt.x = vt->saved_alt.y = 0;
}

void vt_reset(struct vt *vt)
{
	set_altscreen(vt, false, false);
	vt->mode = MODE_AUTOWRAP;
	vt->cursor_style = CURSOR_BLOCK;
	default_cursor(&vt->c);
	vt->saved_pri = vt->saved_alt = vt->c;
	vt->top = 0;
	vt->bot = vt->rows - 1;
	vt->state = ST_GROUND;
	vt->utf8_need = 0;
	vt->lastcp = 0;
	reset_tabs(vt);
	for (int r = 0; r < vt->rows; r++) {
		fill(vt->pri[r], 0, vt->cols, blank(vt));
		fill(vt->alt[r], 0, vt->cols, blank(vt));
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
	}
	if (!vt->pri || !vt->alt || !vt->dirty || !vt->tabs ||
	    (vt->hist_cap && (!vt->hist || !vt->hist_len))) {
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
	free(vt->dirty);
	free(vt->tabs);
	free(vt);
}

/* a new screen holding what fits of old, which is left as it is */
static struct cell **copy_screen(const struct vt *vt, struct cell **old, int shift,
				 int cols, int rows)
{
	struct cell **s = alloc_screen(cols, rows);
	if (!s)
		return NULL;
	struct cell b = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
	for (int r = 0; r < rows && r + shift < vt->rows; r++) {
		memcpy(s[r], old[r + shift], MIN(cols, vt->cols) * sizeof **s);
		if (cols < vt->cols && (s[r][cols - 1].attr & ATTR_WIDE))
			s[r][cols - 1] = b;
	}
	return s;
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
	struct vt_cursor *pc = alt ? &vt->saved_pri : &vt->c;
	struct vt_cursor *ac = alt ? &vt->c : &vt->saved_alt;

	/* keep the cursor line visible: lines above it go into history */
	int pshift = MAX(0, pc->y - (rows - 1));
	int ashift = MAX(0, ac->y - (rows - 1));

	/* everything new first, so a failure leaves the terminal as it was */
	struct cell **npri = copy_screen(vt, vt->pri, pshift, cols, rows);
	struct cell **nalt = copy_screen(vt, vt->alt, ashift, cols, rows);
	bool *ndirty = calloc(rows, sizeof *ndirty);
	bool *ntabs = calloc(cols, sizeof *ntabs);
	if (!npri || !nalt || !ndirty || !ntabs) {
		free_screen(npri, rows);
		free_screen(nalt, rows);
		free(ndirty);
		free(ntabs);
		return false;
	}
	for (int i = 0; i < pshift; i++)
		push_hist(vt, vt->pri[i], vt->cols);
	free_screen(vt->pri, vt->rows);
	free_screen(vt->alt, vt->rows);
	vt->pri = npri;
	vt->alt = nalt;
	vt->line = alt ? vt->alt : vt->pri;
	pc->y -= pshift;
	ac->y -= ashift;
	if (!alt)
		vt->saved_pri.y -= MIN(pshift, vt->saved_pri.y);
	else
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
	clamp_cursor(&vt->c, cols, rows);
	clamp_cursor(&vt->saved_pri, cols, rows);
	clamp_cursor(&vt->saved_alt, cols, rows);
	vt->view = 0;
	vt_mark_all(vt);
	return true;
}

/* ---- printing --------------------------------------------------------- */

static void put(struct vt *vt, uint32_t cp)
{
	struct vt_cursor *c = &vt->c;
	if (cp >= 0x80 && cp < 0xa0)
		return; /* C1 controls are not supported in UTF-8 mode */
	if (c->charset[c->gl] && cp >= 0x5f && cp <= 0x7e)
		cp = dec_graphics[cp - 0x5f];
	int w = charwidth(cp);
	if (w == 0 || w > vt->cols)
		return; /* combining characters are dropped */

	bool wrap = vt->mode & MODE_AUTOWRAP;
	if (wrap && (c->wrapnext || c->x + w > vt->cols)) {
		vt->line[c->y][vt->cols - 1].attr |= ATTR_WRAP;
		c->x = 0;
		index_down(vt);
	} else if (c->x + w > vt->cols) {
		c->x = vt->cols - w;
	}
	c->wrapnext = false;

	struct cell *l = vt->line[c->y];
	if (vt->mode & MODE_INSERT) {
		fix_wide(vt, l, c->x, c->x);
		memmove(&l[c->x + w], &l[c->x], (vt->cols - c->x - w) * sizeof *l);
		if (l[vt->cols - 1].attr & ATTR_WIDE)
			l[vt->cols - 1] = blank(vt);
	}
	fix_wide(vt, l, c->x, c->x + w - 1);
	uint16_t base = c->tmpl.attr & ~(ATTR_WIDE | ATTR_WDUMMY | ATTR_WRAP);
	l[c->x] = c->tmpl;
	l[c->x].cp = cp;
	l[c->x].attr = base | (w == 2 ? ATTR_WIDE : 0);
	if (w == 2) {
		l[c->x + 1] = l[c->x];
		l[c->x + 1].cp = 0;
		l[c->x + 1].attr = base | ATTR_WDUMMY;
	}
	vt->dirty[c->y] = true;
	vt->lastcp = cp;

	c->x += w;
	if (c->x >= vt->cols) {
		c->x = vt->cols - 1;
		c->wrapnext = wrap;
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
		index_down(vt);
		if (vt->mode & MODE_LNM)
			vt->c.x = 0;
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

static void esc_dispatch(struct vt *vt, uint32_t f)
{
	if (vt->ninter == 0) {
		switch (f) {
		case '7': save_cursor(vt); break;
		case '8': restore_cursor(vt); break;
		case 'D': index_down(vt); break;
		case 'E': vt->c.x = 0; index_down(vt); break;
		case 'H': vt->tabs[vt->c.x] = true; break;
		case 'M': reverse_index(vt); break;
		case 'c': vt_reset(vt); break;
		case '=': vt->mode |= MODE_APPKEYPAD; break;
		case '>': vt->mode &= ~MODE_APPKEYPAD; break;
		}
		vt->c.wrapnext = false;
		return;
	}
	switch (vt->inter[0]) {
	case '(':
		vt->c.charset[0] = f == '0';
		break;
	case ')':
		vt->c.charset[1] = f == '0';
		break;
	case '#':
		if (f == '8') { /* DECALN */
			struct cell e = { 'E', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };
			for (int r = 0; r < vt->rows; r++)
				fill(vt->line[r], 0, vt->cols, e);
			vt->top = 0;
			vt->bot = vt->rows - 1;
			moveto(vt, 0, 0);
			vt_mark_all(vt);
		}
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
		case 5: case 6: t->attr |= ATTR_BLINK; break;
		case 7: t->attr |= ATTR_REVERSE; break;
		case 8: t->attr |= ATTR_INVISIBLE; break;
		case 9: t->attr |= ATTR_STRIKE; break;
		case 21: t->attr |= ATTR_UNDERLINE; break;
		case 22: t->attr &= ~(ATTR_BOLD | ATTR_DIM); break;
		case 23: t->attr &= ~ATTR_ITALIC; break;
		case 24: t->attr &= ~ATTR_UNDERLINE; break;
		case 25: t->attr &= ~ATTR_BLINK; break;
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

static void set_private_mode(struct vt *vt, int m, bool on)
{
	unsigned bit = 0;
	switch (m) {
	case 1: bit = MODE_APPCURSOR; break;
	case 6:
		vt->c.origin = on;
		moveto_abs(vt, 0, 0);
		return;
	case 7: bit = MODE_AUTOWRAP; break;
	case 25:
		if (on)
			vt->mode &= ~MODE_HIDECURSOR;
		else
			vt->mode |= MODE_HIDECURSOR;
		return;
	case 47: case 1047:
		set_altscreen(vt, on, m == 1047);
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
	case 1000: case 1002: case 1003:
		vt->mode &= ~MODE_MOUSE;
		bit = m == 1000 ? MODE_MOUSE_BTN :
		      m == 1002 ? MODE_MOUSE_DRAG : MODE_MOUSE_ANY;
		break;
	case 1004: bit = MODE_FOCUS; break;
	case 1006: bit = MODE_MOUSE_SGR; break;
	case 2004: bit = MODE_BRACKETED; break;
	default: return;
	}
	if (on)
		vt->mode |= bit;
	else
		vt->mode &= ~bit;
}

/* DECRQM answer for a private mode: 1 set, 2 reset, 0 unknown. Modes mwt
 * accepts but ignores (12 blink, 1005/1015 mouse encodings) count as unknown,
 * and so does 2027 until grapheme clusters are in: see TODO. */
static int private_mode_state(const struct vt *vt, int m)
{
	unsigned bit;
	switch (m) {
	case 1: bit = MODE_APPCURSOR; break;
	case 6: return vt->c.origin ? 1 : 2;
	case 7: bit = MODE_AUTOWRAP; break;
	case 25: return vt->mode & MODE_HIDECURSOR ? 2 : 1;
	case 47: case 1047: case 1049: bit = MODE_ALTSCREEN; break;
	case 1000: bit = MODE_MOUSE_BTN; break;
	case 1002: bit = MODE_MOUSE_DRAG; break;
	case 1003: bit = MODE_MOUSE_ANY; break;
	case 1004: bit = MODE_FOCUS; break;
	case 1006: bit = MODE_MOUSE_SGR; break;
	case 2004: bit = MODE_BRACKETED; break;
	default: return 0;
	}
	return vt->mode & bit ? 1 : 2;
}

static void csi_dispatch(struct vt *vt, uint32_t f)
{
	struct vt_cursor *c = &vt->c;
	int n = arg(vt, 0, 1);

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
			reply(vt, "\033[?%d;%dR", c->y + 1, c->x + 1);
		return;
	}
	if (vt->priv == '>') {
		if (f == 'c')
			reply(vt, "\033[>1;10;0c");
		else if (f == 'q')
			reply(vt, "\033P>|mwt\033\\");
		return;
	}
	if (vt->priv)
		return;
	if (vt->ninter) {
		if (vt->inter[0] == ' ' && f == 'q') { /* DECSCUSR */
			int s = arg(vt, 0, 0);
			vt->cursor_style = s <= 2 ? CURSOR_BLOCK :
					   s <= 4 ? CURSOR_UNDERLINE : CURSOR_BAR;
		} else if (vt->inter[0] == '!' && f == 'p') {
			soft_reset(vt);
		} else if (vt->inter[0] == '$' && f == 'p') { /* DECRQM, ANSI modes */
			int m = arg(vt, 0, 0);
			unsigned bit = m == 4 ? MODE_INSERT : m == 20 ? MODE_LNM : 0;
			reply(vt, "\033[%d;%d$y", m, !bit ? 0 : vt->mode & bit ? 1 : 2);
		}
		return;
	}

	struct cell *l = vt->line[c->y];
	switch (f) {
	case '@': /* ICH */
		n = MIN(n, vt->cols - c->x);
		fix_wide(vt, l, c->x, c->x);
		memmove(&l[c->x + n], &l[c->x], (vt->cols - c->x - n) * sizeof *l);
		fill(l, c->x, c->x + n, blank(vt));
		if (l[vt->cols - 1].attr & ATTR_WIDE)
			l[vt->cols - 1] = blank(vt);
		vt->dirty[c->y] = true;
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
		case 0:
			clear_line(vt, c->y, c->x, vt->cols - 1);
			for (int r = c->y + 1; r < vt->rows; r++)
				clear_line(vt, r, 0, vt->cols - 1);
			break;
		case 1:
			for (int r = 0; r < c->y; r++)
				clear_line(vt, r, 0, vt->cols - 1);
			clear_line(vt, c->y, 0, c->x);
			break;
		case 2:
			for (int r = 0; r < vt->rows; r++)
				clear_line(vt, r, 0, vt->cols - 1);
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
			scroll_up(vt, c->y, n);
			c->x = 0;
			c->wrapnext = false;
		}
		break;
	case 'P': /* DCH */
		n = MIN(n, vt->cols - c->x);
		fix_wide(vt, l, c->x, c->x + n - 1);
		memmove(&l[c->x], &l[c->x + n], (vt->cols - c->x - n) * sizeof *l);
		fill(l, vt->cols - n, vt->cols, blank(vt));
		vt->dirty[c->y] = true;
		c->wrapnext = false;
		break;
	case 'S': scroll_up(vt, vt->top, n); break;
	case 'T': scroll_down(vt, vt->top, n); break;
	case 'X': clear_line(vt, c->y, c->x, c->x + n - 1); break;
	case 'Z': tab(vt, -n); break;
	case 'b': /* REP */
		if (vt->lastcp)
			for (int i = 0; i < MIN(n, 65535); i++)
				put(vt, vt->lastcp);
		break;
	case 'c':
		if (arg(vt, 0, 0) == 0)
			reply(vt, "\033[?62;22c");
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
			unsigned bit = vt->params[i] == 4 ? MODE_INSERT :
				       vt->params[i] == 20 ? MODE_LNM : 0;
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
			reply(vt, "\033[%d;%dR",
			      c->y + 1 - (c->origin ? vt->top : 0), c->x + 1);
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

static void osc_dispatch(struct vt *vt)
{
	vt->osc[vt->osc_len] = 0;
	char *semi = strchr(vt->osc, ';');
	if (!semi)
		return;
	*semi = 0;
	int num = atoi(vt->osc);
	if ((num == 0 || num == 2) && vt->cb.title)
		vt->cb.title(vt->cb.ud, semi + 1);
}

/* ---- the state machine ------------------------------------------------ */

static void clear_seq(struct vt *vt)
{
	vt->nparams = 0;
	vt->priv = 0;
	vt->ninter = 0;
}

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
	if (cp == ';' || cp == ':') {
		if (vt->nparams < VT_MAX_PARAMS) {
			vt->params[vt->nparams] = 0;
			vt->subparam[vt->nparams] = cp == ':';
			vt->nparams++;
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
	if (vt->osc_len + n < VT_OSC_MAX) {
		memcpy(vt->osc + vt->osc_len, buf, n);
		vt->osc_len += n;
	}
}

static void feed(struct vt *vt, uint32_t cp)
{
	/* "anywhere" transitions */
	if (cp == 0x18 || cp == 0x1a) {
		vt->state = ST_GROUND;
		return;
	}
	if (cp == 0x1b) {
		if (vt->state == ST_OSC)
			osc_dispatch(vt);
		vt->state = ST_ESC;
		clear_seq(vt);
		return;
	}

	switch (vt->state) {
	case ST_GROUND:
		if (cp < 0x20 || cp == 0x7f)
			control(vt, cp);
		else
			put(vt, cp);
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
			vt->state = ST_OSC;
		} else if (cp == 'P' || cp == 'X' || cp == '^' || cp == '_') {
			vt->state = ST_STR_IGNORE; /* DCS, SOS, PM, APC */
		} else if (cp < 0x7f) {
			esc_dispatch(vt, cp);
			vt->state = ST_GROUND;
		} else {
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
		}
		break;
	case ST_CSI_IGNORE:
		if (cp < 0x20)
			control(vt, cp);
		else if (cp >= 0x40 && cp < 0x7f)
			vt->state = ST_GROUND;
		break;
	case ST_OSC:
		if (cp == 0x07) {
			osc_dispatch(vt);
			vt->state = ST_GROUND;
		} else if (cp >= 0x20) {
			osc_put(vt, cp);
		}
		break;
	case ST_STR_IGNORE:
		break;
	}
}

void vt_write(struct vt *vt, const char *buf, size_t len)
{
	const unsigned char *s = (const unsigned char *)buf;
	for (size_t i = 0; i < len; i++) {
		unsigned char b = s[i];
		if (vt->utf8_need) {
			if ((b & 0xc0) == 0x80) {
				vt->utf8_cp = vt->utf8_cp << 6 | (b & 0x3f);
				if (--vt->utf8_need == 0) {
					uint32_t cp = vt->utf8_cp;
					if (cp > 0x10ffff || (cp >= 0xd800 && cp < 0xe000))
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
			vt->utf8_need = 1;
		} else if ((b & 0xf0) == 0xe0) {
			vt->utf8_cp = b & 0x0f;
			vt->utf8_need = 2;
		} else if ((b & 0xf8) == 0xf0 && b <= 0xf4) {
			vt->utf8_cp = b & 0x07;
			vt->utf8_need = 3;
		} else {
			feed(vt, 0xfffd);
		}
	}
}
