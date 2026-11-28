/* replay.c - feed recorded terminal output (VT100 animations, ANSI art,
 * vttest logs) through the VT core and check that nothing breaks.
 *
 * Every file is played three times per grid size: in one write, byte by byte,
 * and in pseudo-random chunks of 1 to 4096 bytes. The three runs must end in
 * the same state (screens, history, cursor, modes, replies), because a pty
 * splits output wherever it likes. After every write the grid must be
 * consistent: cursor and scroll region inside the screen, wide cells in pairs.
 * A fourth run resizes the grid between chunks, as a user dragging the window
 * would; it is only checked for consistency, its result depends on the sizes.
 * Built with ASan/UBSan, see `make replay`.
 *
 * The three runs agreeing does not say they are right: a sequence broken in
 * all of them the same way passes. With -s SUMS the end state of the whole run
 * at 80x24 is hashed and compared with what SUMS holds for the file; -u writes
 * SUMS anew instead. The widths come from the C library's wcwidth, which
 * changes between versions: SUMS records a hash of them, and with other widths
 * the comparison is skipped with a note.
 *
 * With -r it also reports, per file, the sequences mwt ignores on purpose or
 * does not have (double-size lines, 132 columns, blink, ...), so a file that
 * looks wrong on screen can be told apart from a bug.
 *
 * usage: replay [-r] [-q] [-s SUMS [-u]] file...
 * exit:  0 all passed, 1 a check failed, 2 usage or I/O error */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const struct { int cols, rows; } sizes[] = {
	{ 80, 24 },  /* what the files were made for */
	{ 20, 5 },   /* everything clipped */
	{ 132, 50 }, /* larger than expected */
};
#define HIST 200

/* ---- replies -------------------------------------------------------- */

struct run {
	char *reply;
	size_t reply_len, reply_cap;
	int bells;
};

static void on_write(void *ud, const char *b, size_t n)
{
	struct run *r = ud;
	if (r->reply_len + n > r->reply_cap) {
		size_t cap = (r->reply_len + n) * 2;
		char *p = realloc(r->reply, cap);
		if (!p) {
			perror("realloc");
			exit(2);
		}
		r->reply = p;
		r->reply_cap = cap;
	}
	memcpy(r->reply + r->reply_len, b, n);
	r->reply_len += n;
}

static void on_bell(void *ud)
{
	((struct run *)ud)->bells++;
}

/* ---- invariants ------------------------------------------------------ */

static const char *cur_file;
static int cur_cols, cur_rows;
static int failures;

static void fail(const char *how, const char *what, size_t off)
{
	failures++;
	fprintf(stderr, "FAIL %s %dx%d %s: %s (after byte %zu)\n",
		cur_file, cur_cols, cur_rows, how, what, off);
}

/* What is none of a row's content at its end (RESIZE.md 1.1): a blank of its
 * tail, not written. */
static bool end_blank(struct cell c, uint32_t tail)
{
	return c.cp == ' ' && c.fg == COL_DEFAULT_FG && c.bg == tail && !c.attr;
}

/* first problem in the grid, or NULL */
static const char *grid_problem(const struct vt *vt)
{
	if (vt->c.x < 0 || vt->c.x >= vt->cols || vt->c.y < 0 || vt->c.y >= vt->rows)
		return "cursor outside the screen";
	if (vt->top < 0 || vt->bot >= vt->rows || vt->top > vt->bot)
		return "scroll region outside the screen";
	if (vt->c.gl < 0 || vt->c.gl > 1)
		return "GL is neither G0 nor G1";
	/* a history line wraps within what it has */
	for (int i = 0; i < vt->hist_count; i++) {
		int slot = (vt->hist_head - 1 - i + vt->hist_cap) % vt->hist_cap;
		if (vt->hist_wrap[slot] < 0 || vt->hist_wrap[slot] > vt->hist_len[slot])
			return "history line wraps past its end";
		if (vt->hist_tail[slot] != COL_DEFAULT_BG && vt->hist_len[slot] != vt->cols)
			return "history line with a tail in a colour is not full";
		for (int x = vt->hist_wrap[slot]; vt->hist_wrap[slot] && x < vt->hist_len[slot]; x++)
			if (!end_blank(vt->hist[slot][x], vt->hist_tail[slot]))
				return "cell behind a history line's wrap is not of its tail";
	}
	struct row **screens[2] = { vt->pri, vt->alt };
	for (int s = 0; s < 2; s++)
		for (int r = 0; r < vt->rows; r++) {
			const struct cell *l = screens[s][r]->c;
			int used = screens[s][r]->used, wrap = screens[s][r]->wrap;
			if (used < 0 || used > vt->cols) /* blank from here on */
				return "row written past its end";
			if (wrap < 0 || wrap > used)
				return "row wraps past what was written";
			if (screens[s][r]->tail != COL_DEFAULT_BG && used != vt->cols)
				return "row with a tail in a colour is not full";
			/* nothing is checked where a wrap is read (RESIZE.md I2) */
			for (int x = wrap; wrap && x < vt->cols; x++)
				if (!end_blank(l[x], screens[s][r]->tail))
					return "cell behind a row's wrap is not of its tail";
			/* the last row of a screen does not go on (I3) */
			if (r == vt->rows - 1 && wrap)
				return "last row of a screen goes on";
			/* On the main screen a join short of the last column is one
			 * the layout would make: into a row without content or, one
			 * cell short, into one that begins with a wide glyph; from
			 * the history's newest line into the first row too (I8). */
			int join = s ? 0 : r ? screens[s][r - 1]->wrap : vt->hist_count ?
				vt->hist_wrap[(vt->hist_head - 1 + vt->hist_cap) % vt->hist_cap] : 0;
			if (join && join != vt->cols) {
				int n = used;
				while (n > 0 && end_blank(l[n - 1], screens[s][r]->tail))
					n--;
				if ((wrap || n) && !(join == vt->cols - 1 && (l[0].attr & ATTR_WIDE)))
					return "join the layout would not make at this width";
			}
			for (int x = used; x < vt->cols; x++)
				if (l[x].cp != ' ' || l[x].fg != COL_DEFAULT_FG ||
				    l[x].bg != COL_DEFAULT_BG || l[x].attr)
					return "cell after the written part of a row is not blank";
			for (int x = 0; x < vt->cols; x++) {
				bool wide = l[x].attr & ATTR_WIDE;
				bool dummy = l[x].attr & ATTR_WDUMMY;
				if (wide && (x + 1 >= vt->cols || !(l[x + 1].attr & ATTR_WDUMMY)))
					return "wide cell without right half";
				if (dummy && (x == 0 || !(l[x - 1].attr & ATTR_WIDE)))
					return "right half without wide cell";
				if (wide && dummy)
					return "cell is both halves";
				if (l[x].cp >= 0x80 && l[x].cp < 0xa0)
					return "C1 control stored as text";
				if (!(l[x].attr & ATTR_TEXT) &&
				    (l[x].cp != ' ' || l[x].fg != COL_DEFAULT_FG || l[x].attr))
					return "cell that was not written is no blank";
			}
		}
	return NULL;
}

/* ---- comparing runs -------------------------------------------------- */

static bool same_cells(const struct cell *a, const struct cell *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i].cp != b[i].cp || a[i].fg != b[i].fg || a[i].bg != b[i].bg ||
		    a[i].attr != b[i].attr)
			return false;
	return true;
}

/* a row's cells, where its line goes on and what was erased behind it */
static bool same_row(const struct row *a, const struct row *b, int cols)
{
	return a->wrap == b->wrap && a->tail == b->tail && same_cells(a->c, b->c, cols);
}

static bool same_cursor(const struct vt_cursor *a, const struct vt_cursor *b)
{
	return a->x == b->x && a->y == b->y && a->wrapnext == b->wrapnext &&
	       same_cells(&a->tmpl, &b->tmpl, 1) &&
	       a->charset[0] == b->charset[0] && a->charset[1] == b->charset[1] &&
	       a->gl == b->gl && a->origin == b->origin;
}

/* what differs between two terminals fed the same bytes, or NULL */
static const char *difference(const struct vt *a, const struct vt *b)
{
	if (a->mode != b->mode)
		return "modes";
	if (!same_cursor(&a->c, &b->c))
		return "cursor";
	if (!same_cursor(&a->saved_pri, &b->saved_pri) ||
	    !same_cursor(&a->saved_alt, &b->saved_alt))
		return "saved cursor";
	if ((a->mode & MODE_ALTSCREEN) && (a->pri_x != b->pri_x || a->pri_y != b->pri_y))
		return "main screen's cursor behind the alternate one";
	if (a->top != b->top || a->bot != b->bot)
		return "scroll region";
	if (a->cursor_style != b->cursor_style)
		return "cursor style";
	if (memcmp(a->tabs, b->tabs, a->cols * sizeof *a->tabs))
		return "tab stops";
	for (int r = 0; r < a->rows; r++) {
		if (!same_row(a->pri[r], b->pri[r], a->cols))
			return "main screen";
		if (!same_row(a->alt[r], b->alt[r], a->cols))
			return "alternate screen";
	}
	if (a->hist_count != b->hist_count || a->scrolled != b->scrolled)
		return "history length";
	for (int64_t i = vt_abs_first(a); i < vt_abs_first(a) + a->hist_count; i++) {
		int la, lb;
		const struct cell *ca = vt_abs_line(a, i, &la);
		const struct cell *cb = vt_abs_line(b, i, &lb);
		if (!ca || !cb || la != lb || !same_cells(ca, cb, la) ||
		    vt_abs_wraps(a, i) != vt_abs_wraps(b, i))
			return "history";
	}
	return NULL;
}

/* ---- the end state as a hash ------------------------------------------ */

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
	const unsigned char *b = p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 0x100000001b3u;
	return h;
}

/* A line's wrap goes into the hash as the sums have it, from when it was a bit
 * of the last cell before it: 1 << 10 of that cell's attributes. Not ATTR_TEXT,
 * which the sums predate: what was written shows in the other three. */
static uint64_t fnv_cells(uint64_t h, const struct cell *c, int n, int wrap)
{
	for (int i = 0; i < n; i++) {
		uint32_t v[4] = { c[i].cp, c[i].fg, c[i].bg,
				  (c[i].attr & ~ATTR_TEXT) | (i + 1 == wrap) << 10 };
		h = fnv(h, v, sizeof v);
	}
	return h;
}

static uint64_t fnv_cursor(uint64_t h, const struct vt_cursor *c)
{
	int v[7] = { c->x, c->y, c->wrapnext, c->charset[0], c->charset[1], c->gl, c->origin };
	return fnv_cells(fnv(h, v, sizeof v), &c->tmpl, 1, 0);
}

/* what difference() compares, and the replies and bells; not pri_y nor a row's
 * tail, which the sums predate and only a resize reads */
static uint64_t state_hash(const struct vt *vt, const struct run *run)
{
	uint64_t h = 0xcbf29ce484222325u;
	int v[6] = { (int)vt->mode, vt->top, vt->bot, vt->cursor_style, vt->hist_count, run->bells };
	h = fnv(h, v, sizeof v);
	h = fnv(h, &vt->scrolled, sizeof vt->scrolled);
	h = fnv_cursor(fnv_cursor(fnv_cursor(h, &vt->c), &vt->saved_pri), &vt->saved_alt);
	for (int x = 0; x < vt->cols; x++)
		h = fnv(h, &(char){ vt->tabs[x] }, 1);
	for (int r = 0; r < vt->rows; r++)
		h = fnv_cells(fnv_cells(h, vt->pri[r]->c, vt->cols, vt->pri[r]->wrap),
			      vt->alt[r]->c, vt->cols, vt->alt[r]->wrap);
	for (int64_t i = vt_abs_first(vt); i < vt_abs_first(vt) + vt->hist_count; i++) {
		int n;
		const struct cell *l = vt_abs_line(vt, i, &n);
		h = fnv(h, &n, sizeof n);
		if (l)
			h = fnv_cells(h, l, n, vt_abs_wraps(vt, i));
	}
	return fnv(h, run->reply ? run->reply : "", run->reply_len);
}

/* the widths the C library gives, as vt.c asks for them */
static uint64_t width_hash(void)
{
	uint64_t h = 0xcbf29ce484222325u;
	for (wchar_t cp = 0x300; cp < 0x110000; cp++) {
		signed char w = (signed char)wcwidth(cp);
		h = fnv(h, &w, 1);
	}
	return h;
}

struct sum {
	char *name;
	uint64_t hash;
};
static struct sum *sums;
static size_t nsums, sums_cap;
static bool sums_skip, sums_update;
static int sums_bad, sums_new;

/* the name below videos/, so that VT_DIR does not matter */
static const char *sum_name(const char *path)
{
	const char *p = strstr(path, "videos/");
	return p ? p + 7 : path;
}

static struct sum *sum_find(const char *name)
{
	for (size_t i = 0; i < nsums; i++)
		if (!strcmp(sums[i].name, name))
			return &sums[i];
	return NULL;
}

static void sum_add(const char *name, uint64_t hash)
{
	if (nsums == sums_cap) {
		sums_cap = sums_cap ? sums_cap * 2 : 512;
		sums = realloc(sums, sums_cap * sizeof *sums);
		if (!sums) {
			perror("realloc");
			exit(2);
		}
	}
	sums[nsums].name = strdup(name);
	if (!sums[nsums].name) {
		perror("strdup");
		exit(2);
	}
	sums[nsums++].hash = hash;
}

/* SUMS: "widths HASH", then "HASH name" per file */
static bool sums_read(const char *path, uint64_t widths)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return sums_update; /* to be written */
	char line[1024];
	unsigned long long w = 0;
	if (!fgets(line, sizeof line, f) || sscanf(line, "widths %llx", &w) != 1) {
		fprintf(stderr, "replay: %s: no widths line\n", path);
		fclose(f);
		return false;
	}
	if (w != widths && !sums_update) {
		printf("replay: wcwidth here is not the one %s was made with; "
		       "end states not compared\n", path);
		sums_skip = true;
	}
	while (fgets(line, sizeof line, f)) {
		unsigned long long h;
		char name[900];
		if (sscanf(line, "%llx %899[^\n]", &h, name) == 2)
			sum_add(name, h);
	}
	fclose(f);
	return true;
}

static int sum_cmp(const void *a, const void *b)
{
	return strcmp(((const struct sum *)a)->name, ((const struct sum *)b)->name);
}

static bool sums_write(const char *path, uint64_t widths)
{
	qsort(sums, nsums, sizeof *sums, sum_cmp);
	FILE *f = fopen(path, "w");
	if (!f)
		return false;
	fprintf(f, "widths %016llx\n", (unsigned long long)widths);
	for (size_t i = 0; i < nsums; i++)
		fprintf(f, "%016llx %s\n", (unsigned long long)sums[i].hash, sums[i].name);
	return fclose(f) == 0;
}

static void sum_check(uint64_t hash)
{
	const char *name = sum_name(cur_file);
	struct sum *s = sum_find(name);
	if (sums_update) {
		if (s)
			s->hash = hash;
		else
			sum_add(name, hash);
		return;
	}
	if (sums_skip)
		return;
	if (!s) {
		sums_new++;
		printf("replay: %s has no end state in the sums, make replay UPDATE=1\n", name);
	} else if (s->hash != hash) {
		sums_bad++;
		fail("whole", "end state differs from the sums", 0);
	}
}

static bool use_sums;

/* ---- playing --------------------------------------------------------- */

enum { WHOLE, BYTEWISE, RANDOM, RESIZE };
static const char *how_name[] = { "whole", "bytewise", "random", "resize" };

static uint64_t rng;
static unsigned next_rand(void)
{
	rng = rng * 6364136223846793005u + 1442695040888963407u;
	return rng >> 33;
}

/* returns the terminal with run->reply filled, or NULL after a failure */
static struct vt *play(const char *buf, size_t len, int how, struct run *run)
{
	struct vt_callbacks cb = { .write = on_write, .bell = on_bell, .ud = run };
	struct vt *vt = vt_new(cur_cols, cur_rows, HIST, &cb);
	if (!vt) {
		fputs("replay: out of memory\n", stderr);
		exit(2);
	}
	rng = len ^ 0x9e3779b97f4a7c15u;
	size_t off = 0;
	while (off < len) {
		size_t n = how == WHOLE ? len - off :
			   how == BYTEWISE ? 1 : 1 + next_rand() % 4096;
		if (n > len - off)
			n = len - off;
		vt_write(vt, buf + off, n);
		off += n;
		if (how == RESIZE && !vt_resize(vt, 1 + next_rand() % 140, 1 + next_rand() % 60)) {
			fputs("replay: out of memory\n", stderr);
			exit(2);
		}
		/* bytewise checks every 64th byte, or it takes minutes */
		if (how != BYTEWISE || off % 64 == 0 || off == len) {
			const char *p = grid_problem(vt);
			if (p) {
				fail(how_name[how], p, off);
				vt_free(vt);
				return NULL;
			}
		}
	}
	/* scrolling back and forth must not break the history either */
	vt_scroll_view(vt, HIST);
	for (int r = 0; r < vt->rows; r++) {
		int n;
		(void)vt_view_line(vt, r, &n);
	}
	vt_scroll_view(vt, -HIST);
	if (vt->view != 0) {
		fail(how_name[how], "view does not return to live", len);
		vt_free(vt);
		return NULL;
	}
	return vt;
}

static void check_file(const char *buf, size_t len)
{
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
		cur_cols = sizes[s].cols;
		cur_rows = sizes[s].rows;
		struct run runs[4] = { 0 };
		struct vt *vts[4];
		for (int how = 0; how < 4; how++)
			vts[how] = play(buf, len, how, &runs[how]);
		if (use_sums && s == 0 && vts[WHOLE])
			sum_check(state_hash(vts[WHOLE], &runs[WHOLE]));
		for (int how = 1; how < 3; how++) {
			if (!vts[0] || !vts[how])
				continue;
			const char *d = difference(vts[0], vts[how]);
			if (!d && (runs[0].reply_len != runs[how].reply_len ||
				   (runs[0].reply_len &&
				    memcmp(runs[0].reply, runs[how].reply, runs[0].reply_len))))
				d = "replies";
			if (!d && runs[0].bells != runs[how].bells)
				d = "bells";
			if (d) {
				char what[64];
				snprintf(what, sizeof what, "%s differs from whole", d);
				fail(how_name[how], what, len);
			}
		}
		for (int how = 0; how < 4; how++) {
			if (vts[how])
				vt_free(vts[how]);
			free(runs[how].reply);
		}
	}
}

/* ---- report of what mwt does not do ---------------------------------- */

enum {
	R_DOUBLE,  /* ESC # 3/4/6: double height and width lines */
	R_DECCOLM, /* CSI ? 3 h/l: 132 columns */
	R_DECSCNM, /* CSI ? 5 h: reverse screen */
	R_BLINK,   /* SGR 5/6 */
	R_VT52,    /* CSI ? 2 l: VT52 mode */
	R_DECID,   /* ESC Z */
	R_DECALN,  /* ESC # 8: the alignment pattern */
	R_LNM,     /* CSI 20 h: newline mode */
	R_REQTP,   /* CSI x: DECREQTPARM */
	R_DECLL,   /* CSI q: keyboard LEDs */
	R_NATCS,   /* ESC ( or ) with a national set, e.g. UK */
	R_G23,     /* ESC * + . / and SS2/SS3, LS2/LS3 */
	R_8BIT,    /* bytes that are not valid UTF-8 */
	R_C1,      /* C1 controls in UTF-8 (U+0080..U+009F) */
	R_COUNT
};
static const char *rname[R_COUNT] = {
	"double-size", "132-col", "reverse-screen", "blink", "vt52", "decid",
	"decaln", "newline-mode", "decreqtparm", "leds", "national-cs", "g2/g3",
	"not-utf8", "c1",
};
static int rtotal[R_COUNT], rfiles[R_COUNT];

/* bytes that start no valid UTF-8 sequence, or end one too early */
static int invalid_utf8(const unsigned char *s, size_t len)
{
	int bad = 0;
	for (size_t i = 0; i < len;) {
		unsigned c = s[i];
		int need = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 && c >= 0xc2 ? 1 :
			   (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 && c <= 0xf4 ? 3 : -1;
		if (need <= 0) {
			bad += need < 0;
			i++;
			continue;
		}
		int k = 1;
		while (k <= need && i + k < len && (s[i + k] & 0xc0) == 0x80)
			k++;
		bad += k <= need;
		i += k;
	}
	return bad;
}

static void report(const char *name, const char *buf, size_t len)
{
	int cnt[R_COUNT] = { 0 };
	const unsigned char *s = (const unsigned char *)buf;
	for (size_t i = 0; i < len; i++) {
		if (s[i] == 0xc2 && i + 1 < len && s[i + 1] >= 0x80 && s[i + 1] < 0xa0)
			cnt[R_C1]++;
		if (s[i] == 0x0e || s[i] == 0x0f)
			continue; /* SO/SI are supported */
		if (s[i] != 0x1b || i + 1 >= len)
			continue;
		unsigned char f = s[i + 1];
		if (f == '#' && i + 2 < len && (s[i + 2] == '3' || s[i + 2] == '4' || s[i + 2] == '6'))
			cnt[R_DOUBLE]++;
		else if (f == '#' && i + 2 < len && s[i + 2] == '8')
			cnt[R_DECALN]++;
		else if (f == 'Z')
			cnt[R_DECID]++;
		else if ((f == '(' || f == ')') && i + 2 < len && s[i + 2] != '0' && s[i + 2] != 'B')
			cnt[R_NATCS]++;
		else if (f == '*' || f == '+' || f == 'N' || f == 'O' || f == 'n' || f == 'o')
			cnt[R_G23]++;
		else if (f == '[') {
			/* parse CSI: private marker, params, final */
			size_t j = i + 2;
			char priv = 0;
			if (j < len && s[j] >= 0x3c && s[j] <= 0x3f)
				priv = s[j++];
			int params[16], np = 0, v = -1;
			while (j < len && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' || s[j] == ':')) {
				if (s[j] == ';' || s[j] == ':') {
					if (np < 16)
						params[np++] = v < 0 ? 0 : v;
					v = -1;
				} else if (v < 100000) /* capped as in vt.c; only small values matter */
					v = (v < 0 ? 0 : v * 10) + (s[j] - '0');
				j++;
			}
			if (np < 16)
				params[np++] = v < 0 ? 0 : v;
			if (j >= len || (s[j] >= 0x20 && s[j] < 0x30))
				continue; /* intermediates: nothing reported */
			unsigned char fin = s[j];
			for (int k = 0; k < np; k++) {
				if (priv == '?' && (fin == 'h' || fin == 'l')) {
					if (params[k] == 3) cnt[R_DECCOLM]++;
					if (params[k] == 5 && fin == 'h') cnt[R_DECSCNM]++;
					if (params[k] == 2 && fin == 'l') cnt[R_VT52]++;
				} else if (!priv && fin == 'm' && (params[k] == 5 || params[k] == 6))
					cnt[R_BLINK]++;
				else if (!priv && fin == 'h' && params[k] == 20)
					cnt[R_LNM]++;
			}
			if (!priv && fin == 'x')
				cnt[R_REQTP]++;
			if (!priv && fin == 'q')
				cnt[R_DECLL]++;
		}
	}
	cnt[R_8BIT] = invalid_utf8(s, len);

	bool any = false;
	for (int k = 0; k < R_COUNT; k++)
		if (cnt[k]) {
			if (!any)
				printf("%s:", name);
			printf(" %s=%d", rname[k], cnt[k]);
			rtotal[k] += cnt[k];
			rfiles[k]++;
			any = true;
		}
	if (any)
		putchar('\n');
}

/* ---- main ------------------------------------------------------------ */

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	size_t cap = 1 << 16, n = 0;
	char *buf = malloc(cap);
	for (size_t got; buf && (got = fread(buf + n, 1, cap - n, f)) > 0;) {
		n += got;
		if (n == cap) {
			char *p = realloc(buf, cap *= 2);
			if (!p)
				free(buf);
			buf = p;
		}
	}
	bool err = ferror(f);
	fclose(f);
	if (err) {
		free(buf);
		return NULL;
	}
	*len = n;
	return buf;
}

int main(int argc, char **argv)
{
	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "");
	bool do_report = false, quiet = false;
	const char *sums_path = NULL;
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-r"))
			do_report = true;
		else if (!strcmp(argv[i], "-q"))
			quiet = true;
		else if (!strcmp(argv[i], "-u"))
			sums_update = true;
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			sums_path = argv[++i];
		else
			break;
	}
	if (i == argc || (sums_update && !sums_path)) {
		fputs("usage: replay [-r] [-q] [-s SUMS [-u]] file...\n", stderr);
		return 2;
	}
	uint64_t widths = 0;
	if (sums_path) {
		widths = width_hash();
		if (!sums_read(sums_path, widths))
			return 2;
		use_sums = true;
	}
	int files = 0, bad_files = 0;
	size_t bytes = 0;
	for (; i < argc; i++) {
		size_t len;
		char *buf = slurp(argv[i], &len);
		if (!buf) {
			perror(argv[i]);
			return 2;
		}
		cur_file = argv[i];
		int before = failures;
		check_file(buf, len);
		if (failures != before)
			bad_files++;
		else if (!quiet)
			printf("ok %s\n", argv[i]);
		if (do_report)
			report(argv[i], buf, len);
		files++;
		bytes += len;
		free(buf);
	}
	if (do_report) {
		printf("\nnot supported, files (occurrences):\n");
		for (int k = 0; k < R_COUNT; k++)
			if (rfiles[k])
				printf("  %-15s %4d (%d)\n", rname[k], rfiles[k], rtotal[k]);
	}
	printf("replay: %d/%d files passed, %zu bytes, %zu grid sizes\n",
	       files - bad_files, files, bytes, sizeof sizes / sizeof *sizes);
	if (use_sums && sums_update) {
		if (!sums_write(sums_path, widths)) {
			perror(sums_path);
			return 2;
		}
		printf("replay: end states of %zu files written to %s\n", nsums, sums_path);
	} else if (use_sums && !sums_skip) {
		printf("replay: end states at 80x24: %d differ from %s, %d not in it\n",
		       sums_bad, sums_path, sums_new);
	}
	return failures != 0;
}
