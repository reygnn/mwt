/* test_sel.c - the selection over the VT core's lines, headless: make check */
#define _GNU_SOURCE
#include <limits.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sel.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* allocations that fail once, the countdown-th from now; the real ones otherwise */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
static long countdown = -1;
static bool injected;
static bool inject(void)
{
	if (countdown < 0 || countdown-- > 0)
		return false;
	injected = true;
	return true;
}
void *__wrap_malloc(size_t n) { return inject() ? NULL : __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t sz) { return inject() ? NULL : __real_calloc(n, sz); }
void *__wrap_realloc(void *p, size_t n) { return inject() ? NULL : __real_realloc(p, n); }

static uint64_t rng = 0x2545f4914f6cdd1du;
static unsigned rnd(unsigned n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned)(rng % n);
}

static void nop_write(void *ud, const char *b, size_t n) { (void)ud; (void)b; (void)n; }

static struct vt *mk(int cols, int rows, int hist)
{
	struct vt_callbacks cb = { .write = nop_write };
	struct vt *vt = vt_new(cols, rows, hist, &cb);
	if (!vt) {
		fprintf(stderr, "test_sel: no vt\n");
		exit(1);
	}
	return vt;
}

static void w(struct vt *vt, const char *s)
{
	vt_write(vt, s, strlen(s));
}

/* the screen's row r as an absolute line */
static int64_t line(const struct vt *vt, int r)
{
	return (int64_t)vt->scrolled + r;
}

static struct sel sel_of(int mode, int64_t aa, int ac, int64_t ea, int ec)
{
	return (struct sel){ .active = true, .mode = mode, .aa = aa, .ac = ac, .ea = ea, .ec = ec };
}

/* sel_text, compared and freed */
static void check_text(int ln, const struct vt *vt, struct sel s, const char *want)
{
	char *got = sel_text(vt, &s);
	checks++;
	if (got && want && !strcmp(got, want)) {
		free(got);
		return;
	}
	if (!got && !want)
		return;
	failures++;
	fprintf(stderr, "%s:%d: FAIL: selection \"%s\", expected \"%s\"\n", __FILE__, ln,
		got ? got : "(null)", want ? want : "(null)");
	free(got);
}
#define TEXT(vt, s, want) check_text(__LINE__, vt, s, want)

/* the columns of a word selection by a double click at col on row 0 */
static void word_at(struct vt *vt, int col, int *bc, int *ec)
{
	struct sel s = sel_of(SEL_WORD, line(vt, 0), col, line(vt, 0), col);
	int64_t ba, ea;
	sel_range(vt, &s, &ba, bc, &ea, ec);
}
#define WORD(vt, col, b, e) do { int b_, e_; word_at(vt, col, &b_, &e_); \
	checks++; if (b_ != (b) || e_ != (e)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: word at %d is %d..%d, expected %d..%d\n", \
		__FILE__, __LINE__, col, b_, e_, b, e); } } while (0)

static void test_range(void)
{
	struct vt *vt = mk(20, 4, 10);
	int64_t ba, ea;
	int bc, ec;
	/* the end before the anchor: swapped, on one line and across */
	struct sel s = sel_of(SEL_CHAR, 5, 7, 5, 2);
	sel_range(vt, &s, &ba, &bc, &ea, &ec);
	CHECK(ba == 5 && bc == 2 && ea == 5 && ec == 7);
	s = sel_of(SEL_CHAR, 6, 1, 4, 9);
	sel_range(vt, &s, &ba, &bc, &ea, &ec);
	CHECK(ba == 4 && bc == 9 && ea == 6 && ec == 1);
	s = sel_of(SEL_CHAR, 4, 3, 6, 1);
	sel_range(vt, &s, &ba, &bc, &ea, &ec);
	CHECK(ba == 4 && bc == 3 && ea == 6 && ec == 1);
	/* a line selection takes whole lines */
	s = sel_of(SEL_LINE, 6, 4, 4, 9);
	sel_range(vt, &s, &ba, &bc, &ea, &ec);
	CHECK(ba == 4 && bc == 0 && ea == 6 && ec == INT_MAX);
	vt_free(vt);

	vt = mk(30, 4, 10);
	/*          0         1         2
	 *          012345678901234567890123456789 */
	w(vt, "foo bar(baz)   x \"q\" /usr/b:12");
	WORD(vt, 0, 0, 2);
	WORD(vt, 2, 0, 2);
	WORD(vt, 5, 4, 6);
	/* brackets and quotes stand alone */
	WORD(vt, 7, 7, 7);
	WORD(vt, 9, 8, 10);
	WORD(vt, 11, 11, 11);
	WORD(vt, 17, 17, 17);
	WORD(vt, 18, 18, 18);
	/* blanks grow as a word does */
	WORD(vt, 13, 12, 14);
	WORD(vt, 3, 3, 3);
	/* a path with its colon is one word, up to the last column */
	WORD(vt, 23, 21, 29);
	WORD(vt, 29, 21, 29);
	vt_free(vt);

	/* past the end of a short line in the history: blank to the edge */
	vt = mk(10, 3, 10);
	w(vt, "ab\r\n\r\n\r\n\r\n");
	struct sel h = sel_of(SEL_WORD, line(vt, 0) - 2, 5, line(vt, 0) - 2, 5);
	sel_range(vt, &h, &ba, &bc, &ea, &ec);
	CHECK(bc == 2 && ec == 9);
	h.ac = h.ec = 0;
	sel_range(vt, &h, &ba, &bc, &ea, &ec);
	CHECK(bc == 0 && ec == 1);
	vt_free(vt);

	/* beyond ASCII is a word */
	vt = mk(20, 3, 0);
	w(vt, "x gr\xc3\xbc\xc3\x9f" "e y");
	WORD(vt, 3, 2, 6);
	WORD(vt, 7, 7, 7);
	vt_free(vt);

	/* U+0122 ends in the byte of '"', U+012C in that of ','; both are letters */
	vt = mk(20, 3, 0);
	w(vt, "a\xc4\xa2" "b c\xc4\xac" "d");
	WORD(vt, 0, 0, 2);
	WORD(vt, 4, 4, 6);
	vt_free(vt);

	/* other characters stand alone also next to each other; a comma is one */
	vt = mk(20, 3, 0);
	w(vt, "x((y a,b");
	WORD(vt, 2, 2, 2);
	WORD(vt, 1, 1, 1);
	WORD(vt, 5, 5, 5);
	WORD(vt, 6, 6, 6);
	WORD(vt, 7, 7, 7);
	vt_free(vt);
}

/* wide characters: the right half belongs to the left */
static void test_range_wide(void)
{
	struct vt *vt = mk(20, 3, 0);
	/* "a 日本語 b": 日 at 2-3, 本 4-5, 語 6-7 */
	w(vt, "a \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e b");
	WORD(vt, 2, 2, 7);
	WORD(vt, 4, 2, 7);
	WORD(vt, 7, 2, 7);
	WORD(vt, 3, 2, 7);
	TEXT(vt, sel_of(SEL_WORD, line(vt, 0), 5, line(vt, 0), 5), "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
	TEXT(vt, sel_of(SEL_WORD, line(vt, 0), 3, line(vt, 0), 3), "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e");
	/* a blank after a right half stays a blank of its own */
	WORD(vt, 8, 8, 8);
	/* begun on a right half: what is highlighted is copied, 日 with it */
	int64_t l0 = line(vt, 0);
	CHECK(sel_covers(l0, 3, l0, 4, l0, 2, 2));
	TEXT(vt, sel_of(SEL_CHAR, l0, 3, l0, 4), "\xe6\x97\xa5\xe6\x9c\xac");
	TEXT(vt, sel_of(SEL_CHAR, l0, 5, l0, 3), "\xe6\x97\xa5\xe6\x9c\xac");
	TEXT(vt, sel_of(SEL_CHAR, l0, 0, l0, 3), "a \xe6\x97\xa5"); /* ends on a right half */
	TEXT(vt, sel_of(SEL_CHAR, l0, 7, l0, 9), "\xe8\xaa\x9e b");
	vt_free(vt);
}

static void test_covers(void)
{
	/* from line 3 column 4 to line 5 column 2 */
	CHECK(!sel_covers(3, 4, 5, 2, 2, 10, 1));
	CHECK(!sel_covers(3, 4, 5, 2, 3, 3, 1));
	CHECK(sel_covers(3, 4, 5, 2, 3, 4, 1));
	CHECK(sel_covers(3, 4, 5, 2, 3, 79, 1));
	CHECK(sel_covers(3, 4, 5, 2, 4, 0, 1));
	CHECK(sel_covers(3, 4, 5, 2, 5, 2, 1));
	CHECK(!sel_covers(3, 4, 5, 2, 5, 3, 1));
	CHECK(!sel_covers(3, 4, 5, 2, 6, 0, 1));
	/* a wide character in it with either half */
	CHECK(sel_covers(3, 4, 5, 2, 3, 3, 2));
	CHECK(!sel_covers(3, 4, 5, 2, 3, 2, 2));
	CHECK(sel_covers(3, 4, 5, 2, 5, 2, 2));
	/* one cell */
	CHECK(sel_covers(7, 5, 7, 5, 7, 5, 1));
	CHECK(!sel_covers(7, 5, 7, 5, 7, 4, 1));
	CHECK(!sel_covers(7, 5, 7, 5, 7, 6, 1));
	/* whole lines */
	CHECK(sel_covers(7, 0, 8, INT_MAX, 8, 1000, 1));
}

/* A window grown wider keeps the wrap where the line ended, blanks after it:
 * the lines are still one, on the screen as in the history; a cleared screen
 * no longer goes on from the history. */
static void test_text_wider(void)
{
	struct vt *vt = mk(10, 4, 10);
	w(vt, "0123456789abcdefghijklmno");
	CHECK(vt_resize(vt, 20, 4));
	int64_t l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_LINE, l0, 0, l0 + 2, 0), "0123456789abcdefghijklmno");
	TEXT(vt, sel_of(SEL_CHAR, l0, 5, l0 + 1, 15), "56789abcdefghij");
	w(vt, "\r\n\r\n\r\n\r\n");
	int64_t h = vt_abs_first(vt);
	TEXT(vt, sel_of(SEL_LINE, h, 0, h + 2, 0), "0123456789abcdefghijklmno");
	vt_free(vt);

	/* a wrap no longer at the end only after a wider window: DCH empties the
	 * last column, as bash does for Delete in a line that filled the row;
	 * erased to the end, a wrap in the middle goes too; colored blanks after
	 * it are text, the line does not go on */
	static const char *const ends[] = {
		"\033[1;3H\033[P",                     /* 10 columns, DCH */
		"\033[1;3H\033[3P",
		"*\033[1;15H\033[K",                   /* wider, then EL */
		"*\033[1;12H\033[3X\033[1;18H\033[X\033[1;20H\033[X", /* ECH to the end */
		"*\033[41m\033[1;13H\033[3@\033[m",    /* ICH of colored blanks */
	};
	for (size_t i = 0; i < sizeof ends / sizeof *ends; i++) {
		vt = mk(10, 4, 10);
		w(vt, "0123456789abcdefghij");
		const char *e = ends[i];
		if (*e == '*') {
			CHECK(vt_resize(vt, 20, 4));
			e++;
		}
		w(vt, e);
		w(vt, "\033[2;1H\033[2Kfoo");
		l0 = line(vt, 0);
		char *got = sel_text(vt, &(struct sel){ .active = true, .mode = SEL_LINE,
							 .aa = l0, .ea = l0 + 1 });
		checks++;
		if (!got || !strchr(got, '\n')) {
			failures++;
			fprintf(stderr, "%s:%d: FAIL: case %zu joined: \"%s\"\n", __FILE__, __LINE__,
				i, got ? got : "(null)");
		}
		free(got);
		vt_free(vt);
	}
	/* ECH that stops before the end leaves it going on */
	vt = mk(10, 4, 10);
	w(vt, "0123456789abcdefghij");
	CHECK(vt_resize(vt, 20, 4));
	w(vt, "\033[1;12H\033[2X\033[2;1H\033[2Kfoo");
	l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_LINE, l0, 0, l0 + 1, 0), "0123456789foo");
	vt_free(vt);

	vt = mk(10, 3, 10);
	w(vt, "0123456789abcdefghijklmnopqrstuvwxyz");
	w(vt, "\033[2J\033[Hxyz");
	h = vt_abs_first(vt);
	TEXT(vt, sel_of(SEL_LINE, h, 0, line(vt, 0), 0), "0123456789\nxyz");
	vt_free(vt);
}

static void test_text(void)
{
	struct vt *vt = mk(10, 4, 10);
	w(vt, "hello wor\r\nab   \r\n\r\n  x");
	int64_t l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_CHAR, l0, 0, l0, 4), "hello");
	TEXT(vt, sel_of(SEL_CHAR, l0, 4, l0, 0), "hello");
	TEXT(vt, sel_of(SEL_CHAR, l0, 6, l0 + 1, 1), "wor\nab");
	/* trailing blanks go, empty lines stay */
	TEXT(vt, sel_of(SEL_CHAR, l0 + 1, 0, l0 + 3, 9), "ab\n\n  x");
	TEXT(vt, sel_of(SEL_LINE, l0 + 3, 5, l0, 2), "hello wor\nab\n\n  x");
	TEXT(vt, sel_of(SEL_CHAR, l0 + 1, 3, l0 + 1, 4), "");
	/* nothing selected */
	struct sel off = sel_of(SEL_CHAR, l0, 0, l0, 4);
	off.active = false;
	CHECK(sel_text(vt, &off) == NULL);
	vt_free(vt);

	/* a line that wraps joins the next, its blanks kept */
	vt = mk(5, 4, 10);
	w(vt, "abcdefgh\r\nab  cd ef");
	l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_LINE, l0, 0, l0 + 1, 0), "abcdefgh");
	TEXT(vt, sel_of(SEL_LINE, l0 + 2, 0, l0 + 3, 0), "ab  cd ef");
	vt_free(vt);

	/* wide characters once, and cells never written as blanks */
	vt = mk(12, 3, 10);
	w(vt, "\xe6\x97\xa5\xe6\x9c\xac" "a\033[3Cb");
	l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_LINE, l0, 0, l0, 0), "\xe6\x97\xa5\xe6\x9c\xac" "a   b");
	vt_free(vt);

	/* across the history and the screen, past what the history still holds */
	vt = mk(10, 2, 3);
	for (int i = 0; i < 8; i++) {
		char b[16];
		snprintf(b, sizeof b, "line%d\r\n", i);
		w(vt, b);
	}
	w(vt, "end");
	l0 = line(vt, 0);
	TEXT(vt, sel_of(SEL_LINE, l0 - 3, 0, l0 + 1, 0), "line4\nline5\nline6\nline7\nend");
	TEXT(vt, sel_of(SEL_LINE, l0 - 10, 0, l0 - 3, 0), "line4");
	TEXT(vt, sel_of(SEL_LINE, l0 - 10, 0, l0 - 9, 0), "");
	/* a history line is shorter than the screen: the end is clamped */
	TEXT(vt, sel_of(SEL_CHAR, l0 - 1, 2, l0 - 1, 9), "ne6");
	vt_free(vt);
}

static void test_text_nomem(void)
{
	struct vt *vt = mk(80, 30, 0);
	for (int i = 0; i < 29; i++)
		w(vt, "the quick brown fox jumps over the lazy dog, \xc3\xa4\xc3\xb6\xc3\xbc\r\n");
	struct sel s = sel_of(SEL_LINE, line(vt, 0), 0, line(vt, 29), 0);
	int failed = 0;
	for (long n = 0;; n++) {
		countdown = n;
		injected = false;
		char *t = sel_text(vt, &s);
		countdown = -1;
		if (!injected) {
			CHECK(t && strlen(t) == 29 * 52); /* 51 bytes and a newline each */
			free(t);
			break;
		}
		failed++;
		if (t) {
			CHECK(!t);
			free(t);
		}
	}
	CHECK(failed >= 3);
	vt_free(vt);
}

static void test_mark(void)
{
	struct vt *vt = mk(10, 5, 20);
	for (int i = 0; i < 12; i++)
		w(vt, "x\r\n");
	int64_t l0 = line(vt, 0);
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0 + 3, l0 + 1);
	CHECK(!vt->dirty[0] && vt->dirty[1] && vt->dirty[2] && vt->dirty[3] && !vt->dirty[4]);
	/* off the screen: clamped, nothing beyond */
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0 - 5, l0);
	CHECK(vt->dirty[0] && !vt->dirty[1]);
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0 + 4, l0 + 100);
	CHECK(!vt->dirty[3] && vt->dirty[4]);
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0 - 9, l0 - 1);
	sel_mark_lines(vt, l0 + 5, l0 + 9);
	CHECK(!memchr(vt->dirty, 1, 5));
	/* scrolled back by 2: line l0 is shown in row 2 */
	vt_scroll_view(vt, 2);
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0, l0);
	CHECK(!vt->dirty[1] && vt->dirty[2] && !vt->dirty[3]);
	memset(vt->dirty, 0, 5);
	sel_mark_lines(vt, l0 - 2, l0 - 2);
	CHECK(vt->dirty[0] && !vt->dirty[1]);
	vt_free(vt);
}

static void test_sel_mark(void)
{
	struct vt *vt = mk(10, 6, 0);
	int64_t l0 = line(vt, 0);
	struct sel s = { .mode = SEL_CHAR, .aa = l0 + 4, .ac = 2, .ea = l0 + 2, .ec = 7 };
	memset(vt->dirty, 0, 6);
	sel_mark(vt, &s);
	CHECK(!memchr(vt->dirty, 1, 6)); /* not active: nothing shown */
	s.active = true;
	sel_mark(vt, &s);
	CHECK(!vt->dirty[1] && vt->dirty[2] && vt->dirty[3] && vt->dirty[4] && !vt->dirty[5]);
	/* a line selection on one line: that row alone */
	memset(vt->dirty, 0, 6);
	s.mode = SEL_LINE;
	s.aa = s.ea = l0;
	sel_mark(vt, &s);
	CHECK(vt->dirty[0] && !memchr(vt->dirty + 1, 1, 5));
	vt_free(vt);
}

static void test_clicks(void)
{
	struct sel s = { 0 };
	struct clicks c = { 0 };
	enum { D = 400 };
	sel_press(&s, &c, 1000, 7, 3, D);
	CHECK(c.count == 1 && s.mode == SEL_CHAR && !s.active && s.dragging);
	CHECK(s.aa == 7 && s.ea == 7 && s.ac == 3 && s.ec == 3);
	sel_press(&s, &c, 1399, 7, 3, D);
	CHECK(c.count == 2 && s.mode == SEL_WORD && s.active);
	sel_press(&s, &c, 1500, 7, 3, D);
	CHECK(c.count == 3 && s.mode == SEL_LINE && s.active);
	sel_press(&s, &c, 1600, 7, 3, D);
	CHECK(c.count == 1 && s.mode == SEL_CHAR && !s.active);
	/* the time is from the last click, not the first */
	sel_press(&s, &c, 1900, 7, 3, D);
	sel_press(&s, &c, 2250, 7, 3, D);
	CHECK(c.count == 3);
	/* too late, another cell, another line: from one */
	sel_press(&s, &c, 3000, 7, 3, D);
	sel_press(&s, &c, 3400, 7, 3, D);
	CHECK(c.count == 1);
	sel_press(&s, &c, 3500, 7, 4, D);
	CHECK(c.count == 1);
	sel_press(&s, &c, 3600, 8, 4, D);
	CHECK(c.count == 1);
	/* across the wrap of the 32-bit millisecond clock */
	sel_press(&s, &c, 0xffffff00u, 1, 1, D);
	sel_press(&s, &c, 0x10, 1, 1, D);
	CHECK(c.count == 2);
	/* a press starts afresh where it is */
	s = sel_of(SEL_LINE, 1, 2, 9, 9);
	c = (struct clicks){ 0 };
	sel_press(&s, &c, 50000, 4, 5, D);
	CHECK(s.aa == 4 && s.ea == 4 && s.ac == 5 && s.ec == 5 && !s.active);

	/* Shift: the end moves, the anchor and the mode stay */
	s = sel_of(SEL_WORD, 3, 4, 3, 6);
	sel_extend(&s, 9, 1);
	CHECK(s.aa == 3 && s.ac == 4 && s.ea == 9 && s.ec == 1 && s.mode == SEL_WORD &&
	      s.active && s.dragging);
}

static void test_drag(void)
{
	struct vt *vt = mk(10, 8, 0);
	int64_t l0 = line(vt, 0);
	struct sel s = { 0 };
	struct clicks c = { 0 };
	sel_press(&s, &c, 1000, l0 + 2, 3, 400);
	memset(vt->dirty, 0, 8);
	/* the first move makes it active, from the anchor's row to the pointer's */
	CHECK(sel_drag(vt, &s, l0 + 4, 1));
	CHECK(s.active && s.ea == l0 + 4 && s.ec == 1);
	CHECK(!vt->dirty[1] && vt->dirty[2] && vt->dirty[3] && vt->dirty[4] && !vt->dirty[5]);
	/* the same cell again: nothing */
	memset(vt->dirty, 0, 8);
	CHECK(!sel_drag(vt, &s, l0 + 4, 1));
	CHECK(!memchr(vt->dirty, 1, 8));
	/* on: the rows from the old end to the new, and the anchor's */
	CHECK(sel_drag(vt, &s, l0 + 6, 0));
	CHECK(vt->dirty[2] && !vt->dirty[3] && vt->dirty[4] && vt->dirty[5] && vt->dirty[6] &&
	      !vt->dirty[7]);
	/* within the row */
	memset(vt->dirty, 0, 8);
	CHECK(sel_drag(vt, &s, l0 + 6, 5));
	CHECK(vt->dirty[2] && vt->dirty[6] && !vt->dirty[5]);
	vt_free(vt);
}

/* Each row shown whose highlight changed with a drag is marked: the cells
 * sel_covers puts in before and after, over random text, views and drags. */
static void test_drag_marks(void)
{
	static const char *words[] = {
		"foo", "x", "(", ")", "\"", "  ", " ", "\xe6\x97\xa5", "\xc3\xbc" "ber", "a-b",
		"\xe6\x9c\xac\xe8\xaa\x9e",
	};
	enum { COLS = 12, ROWS = 6 };
	long missed = 0, drags = 0;
	for (int round = 0; round < 300; round++) {
		struct vt *vt = mk(COLS, ROWS, 20);
		int lines = 4 + (int)rnd(20);
		for (int i = 0; i < lines; i++) {
			int n = (int)rnd(6);
			for (int k = 0; k < n; k++)
				w(vt, words[rnd(sizeof words / sizeof words[0])]);
			if (i + 1 < lines)
				w(vt, "\r\n");
		}
		vt_scroll_view(vt, (int)rnd(vt->hist_count + 1));
		int64_t top = (int64_t)vt->scrolled - vt->view;
		struct sel s = { 0 };
		struct clicks c = { 0 };
		uint32_t time = 1000;
		for (int step = 0; step < 40; step++) {
			int64_t abs = top + rnd(ROWS);
			int col = (int)rnd(COLS);
			if (rnd(8) == 0 || !s.dragging) {
				/* a press: one, two or three clicks on the cell */
				int n = 1 + (int)rnd(3);
				for (int k = 0; k < n; k++)
					sel_press(&s, &c, time += 10, abs, col, 400);
				time += 1000;
				continue;
			}
			bool before[ROWS][COLS], changed = false;
			int64_t ba, ea;
			int bc, ec;
			sel_range(vt, &s, &ba, &bc, &ea, &ec);
			for (int r = 0; r < ROWS; r++)
				for (int x = 0; x < COLS; x++)
					before[r][x] = s.active && sel_covers(ba, bc, ea, ec, top + r, x, 1);
			memset(vt->dirty, 0, ROWS);
			bool moved = sel_drag(vt, &s, abs, col);
			sel_range(vt, &s, &ba, &bc, &ea, &ec);
			drags++;
			for (int r = 0; r < ROWS; r++) {
				bool differs = false;
				for (int x = 0; x < COLS; x++)
					differs |= before[r][x] != sel_covers(ba, bc, ea, ec, top + r, x, 1);
				changed |= differs;
				if (differs && !vt->dirty[r])
					missed++;
			}
			if (changed && !moved)
				missed++;
		}
		vt_free(vt);
	}
	CHECK(drags > 1000);
	CHECK(missed == 0);
}

static void test_all(void)
{
	struct vt *vt = mk(10, 4, 10);
	struct sel s = { 0 };
	CHECK(!sel_all(vt, &s) && !s.active);
	w(vt, "   \r\n");
	CHECK(!sel_all(vt, &s));
	for (int i = 0; i < 6; i++) {
		char b[16];
		snprintf(b, sizeof b, "l%d\r\n", i);
		w(vt, b);
	}
	/* the history and the screen, down to the last line with text */
	CHECK(sel_all(vt, &s));
	CHECK(s.active && !s.dragging && s.mode == SEL_LINE);
	CHECK(s.aa == vt_abs_first(vt) && s.ea == line(vt, 2));
	TEXT(vt, s, "\nl0\nl1\nl2\nl3\nl4\nl5");
	/* the alternate screen only */
	w(vt, "\033[?1049h\033[Hzwei\r\n\r\ndrei");
	CHECK(sel_all(vt, &s));
	CHECK(s.aa == line(vt, 0) && s.ea == line(vt, 2));
	TEXT(vt, s, "zwei\n\ndrei");
	w(vt, "\033[2J");
	s.active = false;
	CHECK(!sel_all(vt, &s) && !s.active);
	vt_free(vt);
}

int main(void)
{
	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "");
	test_range();
	test_range_wide();
	test_covers();
	test_text();
	test_text_wider();
	test_text_nomem();
	test_mark();
	test_sel_mark();
	test_clicks();
	test_drag();
	test_drag_marks();
	test_all();
	printf("test_sel: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
