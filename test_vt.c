/* test_vt.c - headless tests for the VT core: feed sequences, check the grid */
#define _XOPEN_SOURCE 700
#include "vt.h"
#include "version.h"

#include <limits.h>
#include <stdint.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static int failures, checks;

/* Fault injection, linked with --wrap: with countdown = n the n-th allocation
 * from vt.c (and this file) fails, all others succeed. */
void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t sz);
void *__real_realloc(void *p, size_t n);
static long countdown = -1;
static long reallocs; /* calls of realloc, failed or not */
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
void *__wrap_realloc(void *p, size_t n)
{
	reallocs++;
	return inject() ? NULL : __real_realloc(p, n);
}
static char replybuf[256];
static char titlebuf[VT_OSC_MAX];

static void on_write(void *ud, const char *b, size_t n)
{
	(void)ud;
	/* a reply that does not fit fails a check rather than ASan */
	size_t l = strlen(replybuf), room = sizeof replybuf - 1 - l;
	strncat(replybuf, b, n < room ? n : room);
}

static void on_title(void *ud, const char *t)
{
	(void)ud;
	snprintf(titlebuf, sizeof titlebuf, "%s", t);
}

/* a palette that makes each color easy to tell apart in a reply */
static uint32_t on_color(void *ud, uint32_t c)
{
	(void)ud;
	return c == COL_DEFAULT_FG ? 0xdcdccc : c == COL_DEFAULT_BG ? 0x1e1e1e :
	       c == COL_CURSOR ? 0x00ff7f : c << 16 | 0x0102;
}

static struct vt *mk(int cols, int rows, int hist)
{
	struct vt_callbacks cb = { .write = on_write, .title = on_title, .color = on_color };
	replybuf[0] = titlebuf[0] = 0;
	return vt_new(cols, rows, hist, &cb);
}

static void w(struct vt *vt, const char *s)
{
	vt_write(vt, s, strlen(s));
}

static void enc(char **o, uint32_t cp)
{
	unsigned char *p = (unsigned char *)*o;
	if (cp < 0x80) { *p++ = cp; }
	else if (cp < 0x800) { *p++ = 0xc0 | cp >> 6; *p++ = 0x80 | (cp & 0x3f); }
	else if (cp < 0x10000) { *p++ = 0xe0 | cp >> 12; *p++ = 0x80 | (cp >> 6 & 0x3f); *p++ = 0x80 | (cp & 0x3f); }
	else { *p++ = 0xf0 | cp >> 18; *p++ = 0x80 | (cp >> 12 & 0x3f); *p++ = 0x80 | (cp >> 6 & 0x3f); *p++ = 0x80 | (cp & 0x3f); }
	*o = (char *)p;
}

/* text of a line, trailing blanks trimmed */
static const char *text(const struct cell *l, int len)
{
	static char buf[1024];
	char *o = buf;
	for (int i = 0; i < len; i++)
		if (!(l[i].attr & ATTR_WDUMMY))
			enc(&o, l[i].cp);
	*o = 0;
	while (o > buf && o[-1] == ' ')
		*--o = 0;
	return buf;
}

static const char *row(struct vt *vt, int r)
{
	int len;
	const struct cell *l = vt_view_line(vt, r, &len);
	return l ? text(l, len) : "(null)";
}

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(a, b) do { checks++; const char *_a = (a), *_b = (b); \
	if (strcmp(_a, _b)) { failures++; fprintf(stderr, \
	"%s:%d: FAIL: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #a, _a, _b); } } while (0)

static void test_wrap(void)
{
	struct vt *vt = mk(10, 3, 0);
	w(vt, "hello world!");
	CHECK_STR(row(vt, 0), "hello worl");
	CHECK_STR(row(vt, 1), "d!");
	CHECK(vt->pri[0]->wrap == 10 && vt_abs_wraps(vt, 0) == 10);
	CHECK(vt->c.x == 2 && vt->c.y == 1);
	/* exactly filling a line defers the wrap */
	w(vt, "\r\n0123456789");
	CHECK(vt->c.x == 9 && vt->c.y == 2 && vt->c.wrapnext);
	w(vt, "\r");
	CHECK(!vt->c.wrapnext && vt->c.x == 0);
	/* no autowrap: last column is overwritten */
	w(vt, "\033[?7l\033[1;1Habcdefghijkl");
	CHECK_STR(row(vt, 0), "abcdefghil");
	vt_free(vt);
}

static void test_cursor_erase(void)
{
	struct vt *vt = mk(10, 4, 0);
	w(vt, "aaaaaaaaaa\r\nbbbbbbbbbb\r\ncccccccccc\r\ndddddddddd");
	w(vt, "\033[2;5H\033[K");
	CHECK_STR(row(vt, 1), "bbbb");
	w(vt, "\033[3;5H\033[1K");
	CHECK_STR(row(vt, 2), "     ccccc");
	w(vt, "\033[2;1H\033[J");
	CHECK_STR(row(vt, 0), "aaaaaaaaaa");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 3), "");
	w(vt, "\033[5;20H");
	CHECK(vt->c.x == 9 && vt->c.y == 3);
	w(vt, "\033[2A\033[3D");
	CHECK(vt->c.x == 6 && vt->c.y == 1);
	w(vt, "x\033[2Ky");
	CHECK_STR(row(vt, 1), "       y");
	vt_free(vt);
}

static void test_scroll_history(void)
{
	struct vt *vt = mk(10, 3, 100);
	w(vt, "one\r\ntwo\r\nthree\r\nfour\r\nfive");
	CHECK_STR(row(vt, 0), "three");
	CHECK_STR(row(vt, 2), "five");
	CHECK(vt->hist_count == 2);
	int len;
	const struct cell *first = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK_STR(text(first, len), "one");
	vt_scroll_view(vt, 1);
	CHECK_STR(row(vt, 0), "two");
	CHECK_STR(row(vt, 2), "four");
	/* output while scrolled back keeps the viewed text still */
	w(vt, "\r\nsix");
	CHECK(vt->view == 2);
	CHECK_STR(row(vt, 0), "two");
	vt_scroll_view(vt, -100);
	CHECK_STR(row(vt, 2), "six");
	/* scroll region: lines leaving a region below the top are not kept */
	w(vt, "\033[2;3r\033[3;1H\nX");
	CHECK(vt->hist_count == 3);
	CHECK_STR(row(vt, 0), "four");
	CHECK_STR(row(vt, 1), "six");
	CHECK_STR(row(vt, 2), "X");
	/* ED 3 clears history */
	w(vt, "\033[3J");
	CHECK(vt->hist_count == 0);
	vt_free(vt);
}

static const char *hist_line(struct vt *vt, int back) /* 1: the newest */
{
	int len;
	const struct cell *l = vt_abs_line(vt, (int64_t)vt->scrolled - back, &len);
	return l ? text(l, len) : "(null)";
}

/* A history slot keeps its cells: lines no longer than the one before in
 * their slot go in without realloc, a longer one grows it. */
static void test_history_slots(void)
{
	struct vt *vt = mk(10, 1, 3); /* each line feed pushes the row */
	w(vt, "abcdef\r\nabcdef\r\nabcdef\r\nabcdef\r\n");
	CHECK(vt->hist_count == 3);
	long before = reallocs;
	for (int i = 0; i < 30; i++)
		w(vt, i % 2 ? "xyz\r\n" : "uvwxyz\r\n");
	CHECK(reallocs == before);
	CHECK_STR(hist_line(vt, 1), "xyz");
	CHECK_STR(hist_line(vt, 2), "uvwxyz");
	/* longer than its slot: one realloc */
	w(vt, "0123456789");
	w(vt, "\r\n");
	CHECK(reallocs == before + 1);
	CHECK_STR(hist_line(vt, 1), "0123456789");
	/* a slot that cannot grow keeps its cells and the line goes in empty */
	countdown = 0;
	w(vt, "9876543210\r\n");
	countdown = -1;
	CHECK(vt->hist_count == 3);
	CHECK_STR(hist_line(vt, 1), "");
	CHECK_STR(hist_line(vt, 2), "0123456789");
	w(vt, "ab\r\n");
	CHECK_STR(hist_line(vt, 1), "ab");
	CHECK_STR(hist_line(vt, 2), "");
	CHECK_STR(hist_line(vt, 3), "0123456789");
	vt_free(vt);
}

/* The count of written cells per row (row.used): whatever changes a row must
 * keep it at or above the last cell that is not a default blank, and the history
 * must come out as before it existed. */
static int used(const struct vt *vt, int r) { return vt->line[r]->used; }
static bool used_ok(const struct vt *vt)
{
	struct row **sc[2] = { vt->pri, vt->alt };
	for (int s = 0; s < 2; s++)
		for (int r = 0; r < vt->rows; r++)
			for (int x = sc[s][r]->used; x < vt->cols; x++) {
				struct cell c = sc[s][r]->c[x];
				if (c.cp != ' ' || c.fg != COL_DEFAULT_FG || c.bg != COL_DEFAULT_BG || c.attr)
					return false;
			}
	return true;
}

static void test_row_extent(void)
{
	struct vt *vt = mk(12, 3, 10);
	CHECK(used(vt, 0) == 0);
	w(vt, "abc");
	CHECK(used(vt, 0) == 3);
	/* erasing to the end with default blanks lowers it, in the middle not */
	w(vt, "\033[1;2H\033[K");
	CHECK(used(vt, 0) == 1 && used_ok(vt));
	w(vt, "\033[1;1Habcdef\033[1;2H\033[2X");
	CHECK(used(vt, 0) == 6 && used_ok(vt));
	/* a colored erase is written, all of it (BCE) */
	w(vt, "\033[2;3H\033[41m\033[K\033[m");
	CHECK(used(vt, 1) == 12 && used_ok(vt));
	/* ICH pushes text right, DCH pulls it back, with colored blanks too */
	w(vt, "\033[3;1Hxy\033[1;1H\033[3@");
	CHECK(used(vt, 0) == 9 && used_ok(vt));
	CHECK_STR(row(vt, 0), "   a  def");
	w(vt, "\033[44m\033[2P\033[m");
	CHECK(used(vt, 0) == 12 && used_ok(vt));
	/* insert mode, a wide glyph, a wrap: each raises it */
	w(vt, "\033[3;1H\033[4h\xe6\xbc\xa2\033[4l");
	CHECK(used(vt, 2) == 4 && used_ok(vt));
	w(vt, "\033[3;12Hzq"); /* the wrap scrolls: z's row is full, q's new */
	CHECK(used(vt, 1) == 12 && used(vt, 2) == 1 && vt->line[2]->c[0].cp == 'q');
	/* the history is trimmed the same as before: short lines, colored ones not */
	w(vt, "\033[2J\033[H1\r\n22\r\n\033[42m\033[K\033[m\r\n\n\n\n");
	int len;
	int64_t a = vt_abs_first(vt) + vt->hist_count - 4;
	CHECK(vt_abs_line(vt, a, &len) && len == 1);
	CHECK(vt_abs_line(vt, a + 1, &len) && len == 2);
	CHECK(vt_abs_line(vt, a + 2, &len) && len == 12);
	CHECK(vt_abs_line(vt, a + 3, &len) && len == 0);
	CHECK(used_ok(vt));
	/* the scroll region rotates rows, the count goes with them */
	w(vt, "\033[H\033[2Jtop\r\nmid\033[1;2r\033[1;1H\033M\033[r");
	CHECK(used(vt, 0) == 0 && used(vt, 1) == 3 && used(vt, 2) == 0 && used_ok(vt));
	/* every row written to its end, the alternate screen has its own counts */
	w(vt, "\033[HEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE");
	CHECK(used(vt, 0) == 12 && used(vt, 2) == 12 && used_ok(vt));
	w(vt, "\033[?1049h");
	CHECK(used(vt, 0) == 0 && used_ok(vt));
	w(vt, "alt\033[?1049l");
	CHECK(used(vt, 0) == 12 && used_ok(vt));
	/* a resize lays the rows out anew, and counts what it puts in each */
	CHECK(vt_resize(vt, 5, 3));
	CHECK(used(vt, 0) == 5 && used(vt, 2) == 1 && used_ok(vt));
	CHECK(vt_resize(vt, 20, 4));
	CHECK(vt->c.y == 3 && used(vt, 2) == 20 && used(vt, 3) == 16 && used_ok(vt));
	w(vt, "\033c");
	CHECK(used(vt, 0) == 0 && used_ok(vt));
	vt_free(vt);
}

static void test_scroll_region_ops(void)
{
	struct vt *vt = mk(5, 5, 0);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5");
	w(vt, "\033[2;4r\033[2;1H\033[L");
	CHECK_STR(row(vt, 0), "1");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "2");
	CHECK_STR(row(vt, 3), "3");
	CHECK_STR(row(vt, 4), "5");
	w(vt, "\033[2M");
	CHECK_STR(row(vt, 1), "3");
	CHECK_STR(row(vt, 2), "");
	CHECK_STR(row(vt, 4), "5");
	/* reverse index at top of region scrolls down */
	w(vt, "\033[2;1H\033M");
	CHECK_STR(row(vt, 2), "3");
	/* SU / SD */
	w(vt, "\033[r\033[S");
	CHECK_STR(row(vt, 0), "");
	CHECK_STR(row(vt, 4), "");
	w(vt, "\033[T");
	CHECK_STR(row(vt, 4), "5");
	vt_free(vt);
}

static void test_sgr(void)
{
	struct vt *vt = mk(20, 2, 0);
	w(vt, "\033[1;4;31ma\033[38;2;1;2;3mb\033[38:2::4:5:6mc\033[48;5;100;39md"
	      "\033[38:5:200m\033[22;24me\033[0mf\033[58:2::1:2:3;3mg\033[m\033[94;103mh");
	struct cell *l = vt->pri[0]->c;
	CHECK(l[0].fg == 1 && (l[0].attr & ATTR_BOLD) && (l[0].attr & ATTR_UNDERLINE));
	CHECK(l[1].fg == (COL_RGB | 0x010203));
	CHECK(l[2].fg == (COL_RGB | 0x040506));
	CHECK(l[3].bg == 100 && l[3].fg == COL_DEFAULT_FG);
	CHECK(l[4].fg == 200 && !(l[4].attr & (ATTR_BOLD | ATTR_UNDERLINE)));
	CHECK(l[5].fg == COL_DEFAULT_FG && l[5].bg == COL_DEFAULT_BG && l[5].attr == ATTR_TEXT);
	CHECK((l[6].attr & ATTR_ITALIC) && l[6].fg == COL_DEFAULT_FG);
	CHECK(l[7].fg == 12 && l[7].bg == 11);
	/* background color erase */
	w(vt, "\033[44m\033[2;1H\033[K");
	CHECK(vt->pri[1]->c[10].bg == 4);
	vt_free(vt);
}

static void test_wide(void)
{
	struct vt *vt = mk(6, 2, 0);
	w(vt, "a\xe7\x95\x8c" "b"); /* a 界 b */
	CHECK(vt->pri[0]->c[1].cp == 0x754c && (vt->pri[0]->c[1].attr & ATTR_WIDE));
	CHECK(vt->pri[0]->c[2].attr & ATTR_WDUMMY);
	CHECK(vt->pri[0]->c[3].cp == 'b' && vt->c.x == 4);
	/* overwrite right half: left half becomes blank */
	w(vt, "\033[1;3Hx");
	CHECK(vt->pri[0]->c[1].cp == ' ' && !(vt->pri[0]->c[1].attr & ATTR_WIDE));
	CHECK(vt->pri[0]->c[2].cp == 'x');
	/* wide char at the last column wraps */
	w(vt, "\033[1;6H\xe7\x95\x8c");
	CHECK(vt->pri[1]->c[0].cp == 0x754c && vt->c.y == 1 && vt->c.x == 2);
	vt_free(vt);
}

/* row 0 with W and d for the halves of a wide character */
static const char *shape(struct vt *vt)
{
	static char buf[64];
	int x = 0;
	for (; x < vt->cols && x < (int)sizeof buf - 1; x++) {
		const struct cell *l = &vt->line[0]->c[x];
		buf[x] = l->attr & ATTR_WIDE ? 'W' : l->attr & ATTR_WDUMMY ? 'd' : (char)l->cp;
	}
	buf[x] = 0;
	return buf;
}

static void test_wide_insert(void)
{
#define KANJI "\xe6\xbc\xa2"
	static const struct { const char *seq, *want; } cases[] = {
		/* ICH: on the left half the glyph moves, on the right half it is split */
		{ KANJI KANJI "\033[1;1H\033[@", " WdWd     " },
		{ KANJI KANJI "\033[1;1H\033[2@", "  WdWd    " },
		{ KANJI KANJI "\033[1;2H\033[@", "   Wd     " },
		{ KANJI KANJI "\033[1;3H\033[@", "Wd Wd     " },
		/* ICH: a glyph that no longer fits goes as a whole */
		{ "abcdefg" KANJI "\033[1;1H\033[@", " abcdefgWd" },
		{ "abcdefg" KANJI "\033[1;1H\033[2@", "  abcdefg " },
		{ "abcdefg" KANJI "\033[1;1H\033[3@", "   abcdefg" },
		{ KANJI KANJI "\033[1;1H\033[99@", "          " },
		{ KANJI KANJI "\033[1;2H\033[99@", "          " },
		/* IRM: the same for a narrow and a wide character */
		{ "\033[4h" KANJI KANJI "\033[1;1Hx", "xWdWd     " },
		{ "\033[4h" KANJI KANJI "\033[1;2Hx", " x Wd     " },
		{ "\033[4h" KANJI KANJI "\033[1;1H" KANJI, "WdWdWd    " },
		{ "\033[4h" KANJI KANJI "\033[1;2H" KANJI, " Wd Wd    " },
		{ "\033[4habcdefg" KANJI "\033[1;1Hx", "xabcdefgWd" },
		{ "\033[4habcdefgh" KANJI "\033[1;1Hx", "xabcdefgh " },
		{ "\033[4habcdefgh" KANJI "\033[1;1H" KANJI, "Wdabcdefgh" },
		/* DCH still breaks up what it cuts */
		{ KANJI KANJI "\033[1;1H\033[P", " Wd       " },
		{ KANJI KANJI "\033[1;2H\033[P", " Wd       " },
		{ KANJI KANJI "\033[1;1H\033[3P", "          " },
	};
	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		struct vt *vt = mk(10, 2, 0);
		w(vt, cases[i].seq);
		CHECK_STR(shape(vt), cases[i].want);
		vt_free(vt);
	}
#undef KANJI
}

/* ---- fuzz: random streams, and what must hold whatever comes in ---------- */

static unsigned long long fuzz_state;
static unsigned fuzz_rand(void)
{
	fuzz_state ^= fuzz_state << 13;
	fuzz_state ^= fuzz_state >> 7;
	fuzz_state ^= fuzz_state << 17;
	return (unsigned)(fuzz_state >> 11);
}

/* what a terminal said: a hash of its replies and its last title */
struct fuzz_said {
	unsigned long long replies;
	size_t reply_bytes;
	char title[VT_OSC_MAX + 1];
	int titles;
};
static void fuzz_write(void *ud, const char *b, size_t n)
{
	struct fuzz_said *s = ud;
	for (size_t i = 0; i < n; i++)
		s->replies = (s->replies ^ (unsigned char)b[i]) * 0x100000001b3ull;
	s->reply_bytes += n;
}
static void fuzz_title(void *ud, const char *t)
{
	struct fuzz_said *s = ud;
	snprintf(s->title, sizeof s->title, "%s", t);
	s->titles++;
}

static const char *const fuzz_frags[] = {
	"\033", "\033[", "\033]", "\033P", "\a", "\033\\", "\030", "\032", "\r", "\n",
	"\b", "\t", "\v", "\f", "\x7f", "\016", "\017",
	"\033[K", "\033[1K", "\033[2K", "\033[J", "\033[1J", "\033[2J", "\033[3J",
	"\033[99999C", "\033[99999D", "\033[99999A", "\033[99999B", "\033[0G", "\033[40G",
	"\033[;;;K", "\033[H", "\033[2;3H", "\033[1;2H", "\033[3G", "\033[4G", "\033[D", "\033[C",
	"\033[?1049h", "\033[?1049l", "\033[?47h", "\033[?47l", "\033[?1047h", "\033[?1047l",
	"\033[?6h", "\033[?6l", "\033[?7l", "\033[?7h", "\033[4h", "\033[4l", "\033[20h", "\033[20l",
	"\033[3;5r", "\033[r", "\033[1;1r", "\033[999;999r", "\033[5;2r",
	"\033[L", "\033[M", "\033[99L", "\033[99M", "\033[S", "\033[T", "\033[99S", "\033[99T",
	"\033[P", "\033[@", "\033[X", "\033[2P", "\033[2@", "\033[2X", "\033[99P", "\033[99@", "\033[99X",
	"\033[9b", "\033[Z", "\033[9Z", "\033[9I", "\033[I", "\033[g", "\033[3g",
	"\033H", "\033M", "\033D", "\033E", "\0337", "\0338", "\033c", "\033[!p", "\033#8",
	"\033(0", "\033(B", "\033)0", "\033[6n", "\033[c", "\033[18t", "\033[?2004$p",
	"\033[?6n", "\033[5n", "\033[>c", "\033[>q", "\033[4$p", "\033]2;ab\033\\",
	"\033[1;31m", "\033[38;2;1;2;3m", "\033[38:2::1:2:3m", "\033[38;5m", "\033[38;2m",
	"\033[0m", "\033[7m", "\033[2 q", "\033]0;x\a", "\033]2;\xff\a",
	"\xc3", "\xe2\x82", "\xf0\x9f\x98", "\xff", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
	"\xc3\xa4", "\xe2\x82\xac", "\xcc\x80", "\xe2\x80\x8d",
	/* wide characters often enough for every operation to meet one */
	"\xe6\xbc\xa2", "\xe6\xbc\xa2", "\xe6\xbc\xa2\xe5\xad\x97", "\xe6\xbc\xa2\xe5\xad\x97\xe6\xbc\xa2",
	"\xf0\x9f\x99\x82", "\xf0\x9f\x99\x82",
	"abc", " ", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
};

static size_t fuzz_gen(char *o, size_t cap)
{
	size_t n = 0, nf = sizeof fuzz_frags / sizeof *fuzz_frags;
	while (n + 64 < cap) {
		if (fuzz_rand() % 20 == 0) {
			for (int k = fuzz_rand() % 32; k > 0; k--)
				o[n++] = (char)fuzz_rand();
		} else {
			const char *f = fuzz_frags[fuzz_rand() % nf];
			size_t l = strlen(f);
			memcpy(o + n, f, l);
			n += l;
		}
	}
	return n;
}

/* NULL when all is well, otherwise what is not */
static const char *fuzz_broken_grid(struct row **s, int cols, int rows)
{
	for (int r = 0; r < rows; r++) {
		if (s[r]->wrap < 0 || s[r]->wrap > s[r]->used || s[r]->used > cols)
			return "a row's wrap or its count of written cells out of range";
		if (s[r]->fill != COL_DEFAULT_BG && (s[r]->used != cols || s[r]->wrap))
			return "a row with a fill that is not full, or goes on";
		for (int x = 0; x < cols; x++) {
			unsigned a = s[r]->c[x].attr;
			if ((a & ATTR_WIDE) && (x + 1 >= cols || !(s[r]->c[x + 1].attr & ATTR_WDUMMY)))
				return "left half of a wide character without its right half";
			if ((a & ATTR_WDUMMY) && (x == 0 || !(s[r]->c[x - 1].attr & ATTR_WIDE)))
				return "right half of a wide character without its left half";
			if (!(a & ATTR_TEXT) && (s[r]->c[x].cp != ' ' || s[r]->c[x].fg != COL_DEFAULT_FG || a))
				return "a cell that was not written and is no blank";
		}
	}
	return NULL;
}

static const char *fuzz_broken(const struct vt *vt)
{
	if (vt->c.x < 0 || vt->c.x >= vt->cols || vt->c.y < 0 || vt->c.y >= vt->rows)
		return "cursor outside the grid";
	if (vt->top < 0 || vt->bot >= vt->rows || vt->top > vt->bot)
		return "scroll region outside the grid";
	/* the history is laid out for the screen's width, as the main screen is */
	for (int64_t i = vt_abs_first(vt); i < (int64_t)vt->scrolled; i++) {
		int n;
		vt_abs_line(vt, i, &n);
		if (n > vt->cols)
			return "a history line longer than the screen is wide";
		int slot = (int)((vt->hist_head - ((int64_t)vt->scrolled - i) + vt->hist_cap) % vt->hist_cap);
		if (vt->hist_fill[slot] != COL_DEFAULT_BG && (n != vt->cols || vt->hist_wrap[slot]))
			return "a history line with a fill that is not full, or goes on";
	}
	const char *why = fuzz_broken_grid(vt->pri, vt->cols, vt->rows);
	return why ? why : fuzz_broken_grid(vt->alt, vt->cols, vt->rows);
}

static bool fuzz_cells(const struct cell *a, const struct cell *b, int n)
{
	for (int x = 0; x < n; x++)
		if (a[x].cp != b[x].cp || a[x].fg != b[x].fg || a[x].bg != b[x].bg ||
		    a[x].attr != b[x].attr)
			return false;
	return true;
}

/* a row's cells, where its line goes on and what was erased behind it */
static bool fuzz_rows(const struct row *a, const struct row *b, int cols)
{
	return a->wrap == b->wrap && a->fill == b->fill && fuzz_cells(a->c, b->c, cols);
}

static bool fuzz_cursor(const struct vt_cursor *a, const struct vt_cursor *b)
{
	return a->x == b->x && a->y == b->y && a->wrapnext == b->wrapnext &&
	       fuzz_cells(&a->tmpl, &b->tmpl, 1) && a->charset[0] == b->charset[0] &&
	       a->charset[1] == b->charset[1] && a->gl == b->gl && a->origin == b->origin;
}

/* the whole state, both screens, the history and what each said */
static bool fuzz_same(const struct vt *a, const struct vt *b,
		      const struct fuzz_said *sa, const struct fuzz_said *sb)
{
	if (!fuzz_cursor(&a->c, &b->c) || !fuzz_cursor(&a->saved_pri, &b->saved_pri) ||
	    !fuzz_cursor(&a->saved_alt, &b->saved_alt) || a->mode != b->mode ||
	    a->state != b->state || a->top != b->top || a->bot != b->bot ||
	    a->cursor_style != b->cursor_style || a->hist_count != b->hist_count ||
	    a->scrolled != b->scrolled || memcmp(a->tabs, b->tabs, (size_t)a->cols * sizeof *a->tabs))
		return false;
	if ((a->mode & MODE_ALTSCREEN) && (a->pri_x != b->pri_x || a->pri_y != b->pri_y))
		return false;
	for (int r = 0; r < a->rows; r++)
		if (!fuzz_rows(a->pri[r], b->pri[r], a->cols) || !fuzz_rows(a->alt[r], b->alt[r], a->cols))
			return false;
	for (int64_t i = vt_abs_first(a); i < vt_abs_first(a) + a->hist_count; i++) {
		int la, lb;
		const struct cell *ca = vt_abs_line(a, i, &la), *cb = vt_abs_line(b, i, &lb);
		if (!ca || !cb || la != lb || !fuzz_cells(ca, cb, la) ||
		    vt_abs_wraps(a, i) != vt_abs_wraps(b, i))
			return false;
	}
	return sa->replies == sb->replies && sa->reply_bytes == sb->reply_bytes &&
	       sa->titles == sb->titles && !strcmp(sa->title, sb->title);
}

/* What "the same state" compares after a resize (REFLOW.md, section 5): every
 * row of the main screen with its cells, where its line goes on and what was
 * erased behind it; every row of the history as the row interface gives it;
 * the cursor, the saved cursor and the place behind the alternate screen; the
 * rows in the history and the number of the first one. Not the alternate
 * screen and its cursors, the tab stops, the scroll region and the view,
 * which a resize resets, nor what is to be redrawn. NULL, or what differs.
 *
 * What it can leave out, each until the step of REFLOW.md 8.4 that makes it
 * hold:
 *   SAME_BUT_SAVED  the saved cursor. One never set is carried along with the
 *                   content like one the program saved (D20, step 6).
 *   SAME_BUT_MARKS  ATTR_TEXT of a blank. A reflow marks the erased blanks it
 *                   lays out as text (G3, step 2). */
enum { SAME_BUT_SAVED = 1, SAME_BUT_MARKS = 2 };

static const char *same_cells(const struct cell *a, const struct cell *b, int n, int but)
{
	static char why[48];
	uint16_t mask = but & SAME_BUT_MARKS ? ATTR_TEXT : 0;
	for (int x = 0; x < n; x++) {
		uint16_t m = a[x].cp == ' ' && b[x].cp == ' ' ? mask : 0;
		if (a[x].cp != b[x].cp || a[x].fg != b[x].fg || a[x].bg != b[x].bg ||
		    (a[x].attr | m) != (b[x].attr | m)) {
			snprintf(why, sizeof why, "column %d", x);
			return why;
		}
	}
	return NULL;
}

static const char *same_state(const struct vt *a, const struct vt *b, int but)
{
	static char why[96];
	const char *w;
	if (a->cols != b->cols || a->rows != b->rows)
		return "the size";
	if (a->hist_count != b->hist_count || vt_abs_first(a) != vt_abs_first(b) ||
	    a->scrolled != b->scrolled) {
		snprintf(why, sizeof why, "the history: %d rows from %lld, %d rows from %lld",
			 a->hist_count, (long long)vt_abs_first(a), b->hist_count,
			 (long long)vt_abs_first(b));
		return why;
	}
	/* behind the alternate screen the cursor is that screen's, and the main
	 * one has the place it left */
	bool alt = a->mode & MODE_ALTSCREEN;
	if (alt != !!(b->mode & MODE_ALTSCREEN))
		return "the screen that is shown";
	if (alt ? a->pri_x != b->pri_x || a->pri_y != b->pri_y :
		  a->c.x != b->c.x || a->c.y != b->c.y || a->c.wrapnext != b->c.wrapnext) {
		snprintf(why, sizeof why, "the cursor: %d,%d%s and %d,%d%s", alt ? a->pri_x : a->c.x,
			 alt ? a->pri_y : a->c.y, !alt && a->c.wrapnext ? "+" : "",
			 alt ? b->pri_x : b->c.x, alt ? b->pri_y : b->c.y,
			 !alt && b->c.wrapnext ? "+" : "");
		return why;
	}
	if (!(but & SAME_BUT_SAVED) &&
	    (a->saved_pri.x != b->saved_pri.x || a->saved_pri.y != b->saved_pri.y ||
	     a->saved_pri.wrapnext != b->saved_pri.wrapnext)) {
		snprintf(why, sizeof why, "the saved cursor: %d,%d%s and %d,%d%s", a->saved_pri.x,
			 a->saved_pri.y, a->saved_pri.wrapnext ? "+" : "", b->saved_pri.x,
			 b->saved_pri.y, b->saved_pri.wrapnext ? "+" : "");
		return why;
	}
	for (int r = 0; r < a->rows; r++) {
		const struct row *ra = a->pri[r], *rb = b->pri[r];
		if (ra->wrap != rb->wrap || ra->fill != rb->fill) {
			snprintf(why, sizeof why, "row %d: wrap %d fill %u and wrap %d fill %u", r,
				 ra->wrap, (unsigned)ra->fill, rb->wrap, (unsigned)rb->fill);
			return why;
		}
		if ((w = same_cells(ra->c, rb->c, a->cols, but))) {
			snprintf(why, sizeof why, "row %d, %s", r, w);
			return why;
		}
	}
	for (int64_t i = vt_abs_first(a); i < (int64_t)a->scrolled; i++) {
		int la, lb;
		const struct cell *ca = vt_abs_line(a, i, &la), *cb = vt_abs_line(b, i, &lb);
		long long r = (long long)(i - (int64_t)a->scrolled);
		if (!ca || !cb || la != lb || vt_abs_wraps(a, i) != vt_abs_wraps(b, i) ||
		    vt_abs_fill(a, i) != vt_abs_fill(b, i)) {
			snprintf(why, sizeof why, "history row %lld: %d cells wrap %d and %d cells wrap %d",
				 r, la, vt_abs_wraps(a, i), lb, vt_abs_wraps(b, i));
			return why;
		}
		if ((w = same_cells(ca, cb, la, but))) {
			snprintf(why, sizeof why, "history row %lld, %s", r, w);
			return why;
		}
	}
	return NULL;
}

static void fuzz_report(long seed, const char *what, const char *why)
{
	failures++;
	fprintf(stderr, "fuzz seed %ld: %s: %s\n", seed, what, why);
}

/* REP as xterm 407 has it: only what was printed just before, through DEC
 * graphics again, and its own output not again */
static void test_rep(void)
{
	static const struct { const char *in, *want; } cases[] = {
		{ "a\033[3b", "aaaa" }, { "a\033[b", "aa" }, { "a\033[0b", "aa" },
		{ "a\r\033[3b", "a" }, { "a\033[1m\033[3b", "a" }, { "a\033[C\033[3b", "a" },
		{ "a\0337\0338\033[3b", "a" }, { "a\033]0;t\007\033[3b", "a" },
		{ "ab\033[2D\033[3b", "ab" }, { "a\033[0m\033[3b", "a" },
		{ "a\033[4h\033[3b", "a" }, { "a\033[2b\033[2b", "aaa" },
		{ "a\033[?2b", "a" }, { "a\033[ b", "a" }, { "\033[3b", "" },
		{ "a\033(0\033[3b", "a" }, { "a\x18\033[3b", "a" },
		{ "a\033P1$r\033\\\033[3b", "a" }, { "a\033[1;2<3b\033[3b", "a" },
		{ "\xc3\xa9\033[2b", "\xc3\xa9\xc3\xa9\xc3\xa9" },
		/* a control within a sequence, or a string cut off by ESC, ends
		 * nothing: only what ends in the ground state does */
		{ "a\033[\r3b", "aaa" }, { "a\033\r[3b", "aaa" }, { "a\033[3\007b", "aaaa" },
		{ "a\033Px\033[3b", "aaaa" }, { "a\033]0;t\033[3b", "aaaa" },
		{ "a\033_x\033[3b", "aaaa" }, { "a\033Xx\033[3b", "aaaa" },
		/* a C1 in UTF-8, a format character, a sequence cut off by a
		 * character that is not ASCII end it; a combining mark keeps the base */
		{ "a\xc2\x85\033[3b", "a" }, { "a\xc2\x9c\033[3b", "a" },
		{ "a\xe2\x80\x8b\033[2b", "a" }, { "a\xe2\x80\x8d\033[2b", "a" },
		{ "a\033[\xc3\xa9\033[2b", "a" }, { "e\xcc\x81\033[2b", "eee" },
		/* only the format characters xterm counts as controls end it: of
		 * each range one; other ones of width 0 are marks to xterm */
		{ "a\xe2\x80\xaa\033[2b", "a" }, { "a\xe2\x81\xa0\033[2b", "a" },
		{ "a\xef\xbb\xbf\033[2b", "a" },
		{ "a\xd8\x9c\033[2b", "aaa" }, { "a\xe1\xa0\x8e\033[2b", "aaa" },
		{ "a\xf3\xa0\x80\x81\033[2b", "aaa" },
		/* what has no width to wcwidth() is printed once, not repeated */
		{ "\xe2\x80\xa8\033[2bZ", "\xe2\x80\xa8Z" },
		/* DEL within ESC is ignored, as xterm */
		{ "a\033\x7f[2b", "aaa" },
		/* wide, and at the edge without autowrap */
		{ "\xe7\x95\x8c\033[2b", "\xe7\x95\x8c\xe7\x95\x8c\xe7\x95\x8c" },
		{ "\033[?7l\033[1;11Hab\033[5b", "          ab" },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		struct vt *vt = mk(12, 3, 0);
		w(vt, cases[i].in);
		CHECK_STR(row(vt, 0), cases[i].want);
		vt_free(vt);
	}
	/* DEC graphics: the line drawing character again, not a plain one */
	struct vt *vt = mk(12, 3, 0);
	w(vt, "\033(0q\033[3b");
	bool gfx = true;
	for (int x = 0; x < 4; x++)
		gfx &= vt->pri[0]->c[x].cp == 0x2500 && (vt->pri[0]->c[x].attr & ATTR_DECGFX);
	CHECK(gfx && vt->pri[0]->c[4].cp == ' ');
	vt_free(vt);
	/* across the end of a row */
	vt = mk(12, 3, 0);
	w(vt, "abcdefghijkl\033[2b");
	CHECK_STR(row(vt, 0), "abcdefghijkl");
	CHECK_STR(row(vt, 1), "ll");
	vt_free(vt);
	/* a resize between the character and REP changes nothing */
	vt = mk(4, 2, 0);
	w(vt, "x");
	CHECK(vt_resize(vt, 6, 3));
	w(vt, "\033[2b");
	CHECK_STR(row(vt, 0), "xxx");
	vt_free(vt);
	/* a wide one on a grid now too narrow for it: not printed, nor repeated */
	vt = mk(2, 3, 0);
	w(vt, "\xe7\x95\x8c");
	CHECK(vt_resize(vt, 1, 3));
	w(vt, "\033[5b");
	CHECK(vt->c.y == 0 && vt->c.x == 0);
	w(vt, "\033[2;1Ha\xe7\x95\x8c\033[2b");
	CHECK_STR(row(vt, 1), "a");
	CHECK_STR(row(vt, 2), "");
	vt_free(vt);
	/* the last copy takes the left half of a wide one: its right half goes */
	vt = mk(4, 1, 0);
	w(vt, "\033[1;3H\xe7\x95\x8c\033[Hx\033[2b");
	CHECK(vt->pri[0]->c[2].cp == 'x' && vt->pri[0]->c[3].cp == ' ' && !vt->pri[0]->c[3].attr);
	vt_free(vt);

	/* c ESC [ n b leaves the screen as c n + 1 times does, in any state: insert
	 * mode, wide characters, autowrap off, DEC graphics, a scroll region, origin
	 * mode, a pending wrap; over rows written in these modes */
	static const char *chars[] = { "x", "\xe7\x95\x8c", "q", "\xc3\xa9", "\xf0\x9f\x98\x80" };
	static const int widths[] = { 1, 2, 1, 1, 2 };
	static const char *prep[] = {
		"", "\033[4h", "\033[?7l", "\033(0", "\033[4h\033(0", "\033[4h\033[?7l",
		"\033[2;3r", "\033[4h\033[2;4r", "\033[41m", "\033[4h\033[1;7;44m",
		"\033)0\016", "\033[?1049h\033[4h", "\033[2;3r\033[?6h", "\033[4h\033[1;2r",
	};
	long bad = 0, inserted = 0, cut = 0, pending = 0;
	fuzz_state = 0x2545f4914f6cdd1dULL; /* xorshift stays at 0 from 0 */
	for (int round = 0; round < 4000; round++) {
		int cols = 1 + (int)(fuzz_rand() % (round % 2 ? 40 : 6));
		int rows = 1 + (int)(fuzz_rand() % (round % 2 ? 6 : 3));
		struct vt *a = mk(cols, rows, 20), *b = mk(cols, rows, 20);
		char s[64];
		/* the fill: text, wide ones, colours, moves, insert mode and autowrap */
		for (int i = (int)(fuzz_rand() % (unsigned)(2 * cols * rows + 1)); i > 0; i--) {
			switch (fuzz_rand() % 10) {
			case 0:
				snprintf(s, sizeof s, "\033[%d;%dH", 1 + (int)(fuzz_rand() % (unsigned)rows),
					 1 + (int)(fuzz_rand() % (unsigned)cols));
				break;
			case 1: snprintf(s, sizeof s, "\033[%um", 40 + fuzz_rand() % 3); break;
			case 2: snprintf(s, sizeof s, "\033[4%c", fuzz_rand() % 2 ? 'h' : 'l'); break;
			case 3: snprintf(s, sizeof s, "\033[?7%c", fuzz_rand() % 2 ? 'h' : 'l'); break;
			case 4: case 5: snprintf(s, sizeof s, "\xe7\x95\x8c"); break;
			default: snprintf(s, sizeof s, "%c", 'a' + (int)(fuzz_rand() % 26)); break;
			}
			w(a, s), w(b, s);
		}
		w(a, "\033[0m\033[4l\033[?7h"), w(b, "\033[0m\033[4l\033[?7h");
		const char *p = prep[fuzz_rand() % (sizeof prep / sizeof prep[0])];
		w(a, p), w(b, p);
		snprintf(s, sizeof s, "\033[%d;%dH", 1 + (int)(fuzz_rand() % (unsigned)rows),
			 1 + (int)(fuzz_rand() % (unsigned)cols));
		w(a, s), w(b, s);
		if (fuzz_rand() % 4 == 0) { /* a pending wrap, kept or not by autowrap after */
			snprintf(s, sizeof s, "\033[?7h\033[%dCZ\033[?7%c", cols, fuzz_rand() % 2 ? 'h' : 'l');
			w(a, s), w(b, s);
			pending += a->c.wrapnext;
		}
		int k = (int)(fuzz_rand() % (sizeof chars / sizeof chars[0]));
		const char *c = chars[k];
		w(a, c), w(b, c);
		int n = (int)(fuzz_rand() % (unsigned)(3 * cols * rows + 3));
		int limit = cols * rows + cols / widths[k]; /* past it REP cuts the count */
		memset(a->dirty, 0, (size_t)rows);
		memset(b->dirty, 0, (size_t)rows);
		snprintf(s, sizeof s, "\033[%db", n);
		w(a, s);
		for (int i = 0; i < (n < 1 ? 1 : n); i++)
			w(b, c);
		bool same = fuzz_cursor(&a->c, &b->c) && a->hist_count <= b->hist_count;
		for (int y = 0; y < rows; y++)
			same &= fuzz_rows(a->line[y], b->line[y], cols) &&
				a->line[y]->used == b->line[y]->used && a->dirty[y] >= b->dirty[y];
		if (n <= limit) { /* the history too, line by line */
			same &= a->hist_count == b->hist_count;
			for (int64_t i = vt_abs_first(a); same && i < vt_abs_first(a) + a->hist_count; i++) {
				int la, lb;
				const struct cell *ca = vt_abs_line(a, i, &la), *cb = vt_abs_line(b, i, &lb);
				same = ca && cb && la == lb && fuzz_cells(ca, cb, la) &&
				       vt_abs_wraps(a, i) == vt_abs_wraps(b, i);
			}
		}
		bad += !same;
		inserted += (a->mode & MODE_INSERT) && n > 1;
		cut += n > limit && cols >= widths[k];
		vt_free(a);
		vt_free(b);
	}
	CHECK(bad == 0);
	/* the rounds did reach insert mode, the cut count and a pending wrap */
	CHECK(inserted > 600 && cut > 600 && pending > 300);
}

/* DECSCUSR and DECSTR as xterm 407 */
static void test_xterm_resets(void)
{
	struct vt *vt = mk(10, 4, 0);
	/* DECSCUSR 7 and up keep the style */
	w(vt, "\033[3 q\033[7 q");
	CHECK(vt->cursor_style == CURSOR_UNDERLINE);
	w(vt, "\033[99 q");
	CHECK(vt->cursor_style == CURSOR_UNDERLINE);
	w(vt, "\033[6 q");
	CHECK(vt->cursor_style == CURSOR_BAR);
	/* DECSTR resets it, and redraws the cursor's row */
	memset(vt->dirty, 0, 4);
	w(vt, "\033[!p");
	CHECK(vt->cursor_style == CURSOR_BLOCK && vt->dirty[vt->c.y]);
	vt_free(vt);

	/* DECSTR keeps a pending wrap, and resets only this screen's saved cursor */
	vt = mk(10, 4, 0);
	w(vt, "\033[1;10HZ\033[!pY");
	CHECK(vt->pri[0]->c[9].cp == 'Z' && vt->pri[1]->c[0].cp == 'Y');
	w(vt, "\033[2;5H\0337\033[?1049h\033[!p\033[?1049l\033[4;1H\0338X");
	CHECK(vt->c.y == 1 && vt->c.x == 5 && vt->pri[1]->c[4].cp == 'X');
	vt_free(vt);
	/* the saved cursor it resets keeps a pending wrap, as xterm */
	vt = mk(10, 4, 0);
	w(vt, "\033[1;10HZ\033[!p\0338Y");
	CHECK(vt->pri[0]->c[0].cp == ' ' && vt->pri[1]->c[0].cp == 'Y');
	vt_free(vt);
}

/* VT_FUZZ_SEEDS=100000 ./test_vt for a long run */
static void test_fuzz(void)
{
	const char *env = getenv("VT_FUZZ_SEEDS");
	long seeds = env ? atol(env) : 300;
	static char buf[4000];
	for (long seed = 1; seed <= seeds; seed++) {
		fuzz_state = (unsigned long long)seed * 0x9E3779B97F4A7C15ULL + 1;
		for (int i = 0; i < 5; i++)
			fuzz_rand();
		int cols = 1 + fuzz_rand() % 40, rows = 1 + fuzz_rand() % 12;
		size_t n = fuzz_gen(buf, sizeof buf);
		checks++;
		int before = failures;

		/* the same bytes in one piece and in small ones */
		static struct fuzz_said sw, ss;
		sw = (struct fuzz_said){ .replies = 0xcbf29ce484222325ull };
		ss = sw;
		struct vt_callbacks cbw = { .write = fuzz_write, .title = fuzz_title, .ud = &sw };
		struct vt_callbacks cbs = { .write = fuzz_write, .title = fuzz_title, .ud = &ss };
		struct vt *whole = vt_new(cols, rows, 50, &cbw), *split = vt_new(cols, rows, 50, &cbs);
		vt_write(whole, buf, n);
		for (size_t i = 0; i < n;) {
			size_t k = 1 + fuzz_rand() % 7;
			k = k > n - i ? n - i : k;
			vt_write(split, buf + i, k);
			i += k;
		}
		if (!fuzz_same(whole, split, &sw, &ss))
			fuzz_report(seed, "chunks", "the result depends on how the input is cut");
		vt_free(whole);
		vt_free(split);

		/* in pieces, with what a window does in between */
		static struct fuzz_said sv;
		struct vt_callbacks cbv = { .write = fuzz_write, .title = fuzz_title, .ud = &sv };
		struct vt *vt = vt_new(cols, rows, 50, &cbv);
		for (size_t i = 0; i < n && failures == before;) {
			size_t k = 1 + fuzz_rand() % 600;
			k = k > n - i ? n - i : k;
			vt_write(vt, buf + i, k);
			i += k;
			const char *why = fuzz_broken(vt);
			if (why) {
				fuzz_report(seed, "after output", why);
				break;
			}
			vt_resize(vt, 1 + fuzz_rand() % 60, 1 + fuzz_rand() % 16);
			why = fuzz_broken(vt);
			if (why) {
				fuzz_report(seed, "after a resize", why);
				break;
			}
			vt_scroll_view(vt, (int)(fuzz_rand() % 100) - 50);
			for (int r = 0; r < vt->rows; r++) {
				int len;
				vt_view_line(vt, r, &len);
			}
			if (fuzz_rand() % 8 == 0)
				vt_reset(vt);
		}
		vt_free(vt);
	}
}

static void test_altscreen(void)
{
	struct vt *vt = mk(10, 3, 10);
	w(vt, "main\033[2;3H");
	w(vt, "\033[?1049h");
	CHECK(vt->mode & MODE_ALTSCREEN);
	CHECK_STR(row(vt, 0), "");
	w(vt, "\033[Halt\r\n\n\n\n\n");
	CHECK(vt->hist_count == 0); /* alt screen never feeds history */
	w(vt, "\033[?1049l");
	CHECK_STR(row(vt, 0), "main");
	CHECK(vt->c.x == 2 && vt->c.y == 1);
	vt_free(vt);
}

static void test_replies(void)
{
	struct vt *vt = mk(10, 5, 0);
	w(vt, "\033[3;4H\033[6n");
	CHECK_STR(replybuf, "\033[3;4R");
	replybuf[0] = 0;
	w(vt, "\033[c");
	CHECK_STR(replybuf, "\033[?62;22c");
	replybuf[0] = 0;
	w(vt, "\033[>q");
	CHECK_STR(replybuf, "\033P>|mwt(" MWT_VERSION ")\033\\");
	replybuf[0] = 0;
	w(vt, "\033[>0q");
	CHECK_STR(replybuf, "\033P>|mwt(" MWT_VERSION ")\033\\");
	replybuf[0] = 0;
	w(vt, "\033[>c");
	CHECK_STR(replybuf, "\033[>1;10;0c");
	replybuf[0] = 0;
	w(vt, "\033[>0c");
	CHECK_STR(replybuf, "\033[>1;10;0c");
	replybuf[0] = 0;
	/* other parameters ask nothing, as in xterm */
	w(vt, "\033[>1c\033[>1q");
	CHECK_STR(replybuf, "");
	w(vt, "\033[18t");
	CHECK_STR(replybuf, "\033[8;5;10t");
	w(vt, "\033]0;hello\a");
	CHECK_STR(titlebuf, "hello");
	w(vt, "\033]2;w\xc3\xa4rld\033\\");
	CHECK_STR(titlebuf, "w\xc3\xa4rld");
	/* DECRQM: set, reset, unknown; 2027 stays unknown until it is in */
	replybuf[0] = 0;
	w(vt, "\033[?2004h\033[?2004$p\033[?1049$p\033[?2027$p\033[?25$p");
	CHECK_STR(replybuf, "\033[?2004;1$y\033[?1049;2$y\033[?2027;0$y\033[?25;1$y");
	replybuf[0] = 0;
	w(vt, "\033[4h\033[4$p\033[20$p\033[99$p\033[4l"); /* 20, LNM: never set */
	CHECK_STR(replybuf, "\033[4;1$y\033[20;4$y\033[99;0$y");
	/* 4: modes mwt knows of but does not have, so setting them never takes */
	replybuf[0] = 0;
	w(vt, "\033[?3h\033[?3$p\033[?5$p\033[?12$p\033[?45$p\033[?69$p\033[?1005$p");
	CHECK_STR(replybuf, "\033[?3;4$y\033[?5;4$y\033[?12;4$y\033[?45;4$y"
			    "\033[?69;4$y\033[?1005;4$y");
	replybuf[0] = 0;
	w(vt, "\033[2$p\033[19$p\033[12$p\033[21$p");
	CHECK_STR(replybuf, "\033[2;4$y\033[19;4$y\033[12;0$y\033[21;0$y");
	/* 3: 1035 is always on, the keypad sends the same in any mode; turning it
	 * off does not take */
	replybuf[0] = 0;
	w(vt, "\033[?1035$p\033[?1035l\033[?1035$p\033=\033[?1035$p\033>");
	CHECK_STR(replybuf, "\033[?1035;3$y\033[?1035;3$y\033[?1035;3$y");
	/* DECNKM is the keypad mode of ESC = and ESC > */
	replybuf[0] = 0;
	w(vt, "\033[?66$p\033=\033[?66$p\033>\033[?66h");
	CHECK(vt->mode & MODE_APPKEYPAD);
	w(vt, "\033[?66l");
	CHECK(!(vt->mode & MODE_APPKEYPAD));
	CHECK_STR(replybuf, "\033[?66;2$y\033[?66;1$y");
	/* a private mode with an intermediate is not a set or reset */
	w(vt, "\033[?2004$h\033[?2004$l");
	CHECK(vt->mode & MODE_BRACKETED);
	w(vt, "\033[?2004l");
	/* DCS strings are swallowed without output */
	w(vt, "\033P+q544e\033\\ok");
	CHECK_STR(row(vt, 2), "   ok");
	vt_free(vt);
}

/* vt_key: the bytes of the keys tests/keys.py checks under sway, here without a
 * display; plus DECCKM, all F keys and the edge cases. */
static const char *key(struct vt *vt, enum vt_key k, unsigned mods)
{
	static char buf[17];
	size_t n = vt_key(vt, k, mods, buf, 16);
	buf[n] = 0;
	return buf;
}

static void test_keys(void)
{
	struct vt *vt = mk(10, 3, 0);
	enum { S = VT_MOD_SHIFT, A = VT_MOD_ALT, C = VT_MOD_CTRL };
	static const struct { enum vt_key k; unsigned mods; const char *want; } cases[] = {
		{ VT_KEY_UP, 0, "\033[A" }, { VT_KEY_DOWN, 0, "\033[B" },
		{ VT_KEY_RIGHT, 0, "\033[C" }, { VT_KEY_LEFT, 0, "\033[D" },
		{ VT_KEY_HOME, 0, "\033[H" }, { VT_KEY_END, 0, "\033[F" },
		{ VT_KEY_UP, C, "\033[1;5A" }, { VT_KEY_LEFT, A, "\033[1;3D" },
		{ VT_KEY_RIGHT, S | A, "\033[1;4C" }, { VT_KEY_DOWN, C | A, "\033[1;7B" },
		{ VT_KEY_END, S | A | C, "\033[1;8F" },
		{ VT_KEY_BEGIN, 0, "\033[E" }, { VT_KEY_BEGIN, C, "\033[1;5E" },
		{ VT_KEY_MENU, 0, "\033[29~" }, { VT_KEY_MENU, S, "\033[29;2~" },
		{ VT_KEY_INSERT, 0, "\033[2~" }, { VT_KEY_DELETE, C, "\033[3;5~" },
		{ VT_KEY_PAGE_UP, C, "\033[5;5~" }, { VT_KEY_PAGE_DOWN, 0, "\033[6~" },
		{ VT_KEY_F1, 0, "\033OP" }, { VT_KEY_F1, C, "\033[1;5P" },
		{ VT_KEY_F4, S | A, "\033[1;4S" }, { VT_KEY_F5, A, "\033[15;3~" },
		{ VT_KEY_F6, 0, "\033[17~" }, { VT_KEY_F10, 0, "\033[21~" },
		{ VT_KEY_F11, 0, "\033[23~" }, { VT_KEY_F12, S, "\033[24;2~" },
		{ VT_KEY_ENTER, 0, "\r" }, { VT_KEY_ENTER, A, "\033\r" },
		{ VT_KEY_TAB, 0, "\t" }, { VT_KEY_TAB, S, "\033[Z" }, { VT_KEY_TAB, A, "\033\t" },
		{ VT_KEY_BACKTAB, 0, "\033[Z" }, { VT_KEY_BACKTAB, A, "\033[Z" },
		{ VT_KEY_ESCAPE, 0, "\033" }, { VT_KEY_ESCAPE, A, "\033\033" },
		{ VT_KEY_BACKSPACE, 0, "\x7f" }, { VT_KEY_BACKSPACE, C, "\b" },
		{ VT_KEY_BACKSPACE, A, "\033\x7f" },
	};
	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++)
		CHECK_STR(key(vt, cases[i].k, cases[i].mods), cases[i].want);
	/* DECCKM: SS3 without modifiers, CSI 1 ; m with them, as xterm */
	w(vt, "\033[?1h");
	CHECK_STR(key(vt, VT_KEY_UP, 0), "\033OA");
	CHECK_STR(key(vt, VT_KEY_HOME, 0), "\033OH");
	CHECK_STR(key(vt, VT_KEY_BEGIN, 0), "\033OE");
	CHECK_STR(key(vt, VT_KEY_MENU, 0), "\033[29~");
	CHECK_STR(key(vt, VT_KEY_UP, S), "\033[1;2A");
	CHECK_STR(key(vt, VT_KEY_INSERT, 0), "\033[2~");
	/* the keypad mode changes nothing, see CAVEATS.md */
	w(vt, "\033[?1l\033=");
	CHECK_STR(key(vt, VT_KEY_ENTER, 0), "\r");
	CHECK_STR(key(vt, VT_KEY_ENTER, A), "\033\r");
	CHECK_STR(key(vt, VT_KEY_UP, 0), "\033[A");
	/* no room, no key: nothing */
	char small[3];
	CHECK(vt_key(vt, VT_KEY_UP, 0, small, sizeof small) == 3);
	CHECK(vt_key(vt, VT_KEY_UP, C, small, sizeof small) == 0);
	CHECK(vt_key(vt, VT_KEY_COUNT, 0, small, sizeof small) == 0);
	CHECK(vt_key(vt, (enum vt_key)-1, 0, small, sizeof small) == 0);
	/* the longest there is, ESC [ 2 4 ; 8 ~, fits in 16 */
	char big[16];
	CHECK(vt_key(vt, VT_KEY_F12, S | A | C, big, sizeof big) == 7);
	vt_free(vt);
}

/* OSC 4, 10, 11 and 12 queries: answered with the colors as drawn, in xterm's
 * form, ended as the question was; setting a color is ignored. */
static void test_color_queries(void)
{
	struct vt *vt = mk(10, 3, 0);
	w(vt, "\033]11;?\033\\");
	CHECK_STR(replybuf, "\033]11;rgb:1e1e/1e1e/1e1e\033\\");
	replybuf[0] = 0;
	w(vt, "\033]10;?\a");
	CHECK_STR(replybuf, "\033]10;rgb:dcdc/dcdc/cccc\a");
	/* each further entry is the next number: 10, 11, 12 */
	replybuf[0] = 0;
	w(vt, "\033]10;?;?;?\a");
	CHECK_STR(replybuf, "\033]10;rgb:dcdc/dcdc/cccc\a\033]11;rgb:1e1e/1e1e/1e1e\a"
			    "\033]12;rgb:0000/ffff/7f7f\a");
	/* a set in the list is skipped, a query after it still answered */
	replybuf[0] = 0;
	w(vt, "\033]10;#ffffff;?\a");
	CHECK_STR(replybuf, "\033]11;rgb:1e1e/1e1e/1e1e\a");
	/* palette entries, pairs of index and spec */
	replybuf[0] = 0;
	w(vt, "\033]4;1;?;255;?\033\\");
	CHECK_STR(replybuf, "\033]4;1;rgb:0101/0101/0202\033\\\033]4;255;rgb:ffff/0101/0202\033\\");
	/* nothing for what is not a query or out of range */
	replybuf[0] = 0;
	w(vt, "\033]4;1;red\a\033]4;256;?\a\033]4;x;?\a\033]4;1\a\033]11;\a"
	      "\033]13;?\a\033]104\a\033]4x;1;?\a");
	CHECK_STR(replybuf, "");
	/* a query does not touch the screen or the title */
	w(vt, "\033]0;t\aok");
	CHECK_STR(row(vt, 0), "ok");
	CHECK_STR(titlebuf, "t");
	vt_free(vt);
	/* without the callback no answer */
	struct vt_callbacks cb = { .write = on_write };
	vt = vt_new(10, 3, 0, &cb);
	replybuf[0] = 0;
	w(vt, "\033]11;?\a\033]4;1;?\a");
	CHECK_STR(replybuf, "");
	vt_free(vt);
}

/* DECRQCRA. Each sum worked out by hand: the codes of the cells plus their
 * attribute bits, negated, in 16 bits; an erased cell is a blank, 0x20. */
static void test_checksum(void)
{
	struct vt *vt = mk(10, 5, 0);
	w(vt, "AB\033[1;1;1;1;1;2*y");
	CHECK_STR(replybuf, ""); /* off unless allowed */
	vt->allow_checksum = true;
	w(vt, "\033[2;1;1;1;1;2*y"); /* 'A' + 'B' = 0x83 */
	CHECK_STR(replybuf, "\033P2!~FF7D\033\\");
	replybuf[0] = 0;
	w(vt, "\033[3;1;1;2;1;2*y"); /* only column 2: 'B' = 0x42 */
	CHECK_STR(replybuf, "\033P3!~FFBE\033\\");
	replybuf[0] = 0;
	w(vt, "\033[1;1;3;1;2;10*y"); /* top below bottom: nothing */
	CHECK_STR(replybuf, "\033P1!~0000\033\\");
	replybuf[0] = 0;
	w(vt, "\b\033[K\033[1;1;1;1;1;2*y"); /* erased is a blank: 'A' + 0x20 */
	CHECK_STR(replybuf, "\033P1!~FF9F\033\\");
	replybuf[0] = 0;
	w(vt, "\033[1;1;1;1;5;10*y"); /* the whole screen: 'A' + 49 blanks = 0x661 */
	CHECK_STR(replybuf, "\033P1!~F99F\033\\");
	replybuf[0] = 0;
	w(vt, "\033[2J\033[H\033[1mA\033[0m\033[1;1;1;1;1;1*y"); /* bold: 0x41 + 0x80 */
	CHECK_STR(replybuf, "\033P1!~FF3F\033\\");
	replybuf[0] = 0;
	w(vt, "\033[2J\033[H\033[4;7mA\033[0m\033[1;1;1;1;1;1*y"); /* + 0x10 + 0x20 */
	CHECK_STR(replybuf, "\033P1!~FF8F\033\\");
	replybuf[0] = 0;
	w(vt, "\033[2J\033[H\033(0q\033(B\033[1;1;1;1;1;1*y"); /* DEC graphics: 'q' */
	CHECK_STR(replybuf, "\033P1!~FF8F\033\\");
	replybuf[0] = 0;
	w(vt, "\033[2J\033[H\xe6\xbc\xa2\033[1;1;1;1;1;2*y"); /* wide: ESC for each half */
	CHECK_STR(replybuf, "\033P1!~FFCA\033\\");
	replybuf[0] = 0;
	w(vt, "\033[2J\033[H\xc3\xa4\033[1;1;1;1;1;1*y"); /* Latin-1 cut to 7 bits: 0x64 */
	CHECK_STR(replybuf, "\033P1!~FF9C\033\\");
	replybuf[0] = 0;
	/* origin mode: rows count from the top margin */
	w(vt, "\033[2J\033[2;4r\033[?6h\033[HC\033[1;1;1;1;1;1*y");
	CHECK_STR(replybuf, "\033P1!~FFBD\033\\");
	replybuf[0] = 0;
	w(vt, "\033[?6l\033[r\033[1;1;1;1;1;1*y"); /* row 1 holds a blank */
	CHECK_STR(replybuf, "\033P1!~FFE0\033\\");
	vt_free(vt);
}

/* The sum over a large screen passes INT_MAX: 4096x1500 cells of 374 each */
static void test_checksum_large(void)
{
	struct vt *vt = mk(4096, 1500, 0);
	vt->allow_checksum = true;
	size_t n = (size_t)4096 * 1500;
	char *s = malloc(n + 1);
	CHECK(s != NULL);
	if (!s) {
		vt_free(vt);
		return;
	}
	memset(s, '~', n);
	s[n] = 0;
	w(vt, "\033[1;4;5;7;8m"); /* '~' 0x7e plus 0x08 0x10 0x20 0x40 0x80 */
	w(vt, s);
	w(vt, "\033[0m\033[1*y");
	CHECK_STR(replybuf, "\033P1!~8000\033\\");
	free(s);
	vt_free(vt);
}

static void test_osc_controls(void)
{
	/* DEL and C1 never get into a title, which goes on to the compositor */
	struct vt *vt = mk(10, 2, 0);
	w(vt, "\033]2;a\x7f" "b\xc2\x85" "c\xc2\x9b" "d\xc3\xa4\a");
	CHECK_STR(titlebuf, "abcd\xc3\xa4");
	vt_free(vt);
}

/* An OSC holds VT_OSC_MAX - 1 bytes, its number and the ';' among them. */
static void test_osc_long(void)
{
	struct vt *vt = mk(10, 2, 0);
	char seq[VT_OSC_MAX + 64], *o = seq;
	/* a title that fills it to the last byte is whole */
	o += sprintf(o, "\033]2;");
	memset(o, 'a', VT_OSC_MAX - 3);
	o += VT_OSC_MAX - 3;
	*o++ = '\a';
	vt_write(vt, seq, (size_t)(o - seq));
	CHECK(strlen(titlebuf) == VT_OSC_MAX - 3);
	/* one that does not fit is cut at a whole character, and for good: the Z
	 * after the character that did not fit would leave a hole */
	o = seq + sprintf(seq, "\033]2;");
	memset(o, 'b', VT_OSC_MAX - 5);
	o += VT_OSC_MAX - 5;
	o += sprintf(o, "\xe2\x82\xacZ\a");
	vt_write(vt, seq, (size_t)(o - seq));
	CHECK(strlen(titlebuf) == VT_OSC_MAX - 5);
	CHECK(titlebuf[VT_OSC_MAX - 6] == 'b');
	/* a query that does not fit is not answered in part */
	o = seq + sprintf(seq, "\033]4");
	while (o - seq < VT_OSC_MAX + 16)
		o += sprintf(o, ";1;?");
	*o++ = '\a';
	replybuf[0] = 0;
	vt_write(vt, seq, (size_t)(o - seq));
	CHECK_STR(replybuf, "");
	/* and the next OSC starts anew */
	w(vt, "\033]2;ok\a\033]4;1;?\a");
	CHECK_STR(titlebuf, "ok");
	CHECK_STR(replybuf, "\033]4;1;rgb:0101/0101/0202\a");
	vt_free(vt);
}

static void test_charset_and_misc(void)
{
	struct vt *vt = mk(10, 3, 0);
	w(vt, "\033(0lqk\033(Bq");
	CHECK(vt->pri[0]->c[0].cp == 0x250c && vt->pri[0]->c[1].cp == 0x2500);
	CHECK(vt->pri[0]->c[3].cp == 'q');
	w(vt, "\r\nab\033[3b");
	CHECK_STR(row(vt, 1), "abbbb");
	w(vt, "\033[1;1H\033[2P");
	CHECK(vt->pri[0]->c[0].cp == 0x2510 && vt->pri[0]->c[1].cp == 'q');
	w(vt, "\033[2;2H\033[2@");
	CHECK_STR(row(vt, 1), "a  bbbb");
	w(vt, "\033[4h\033[2;1HZ\033[4l");
	CHECK_STR(row(vt, 1), "Za  bbbb");
	w(vt, "\033[2;1H\033[3X");
	CHECK_STR(row(vt, 1), "    bbbb");
	/* tabs */
	w(vt, "\033[3;1H\tT\033[Z\033[Z");
	CHECK(vt->pri[2]->c[8].cp == 'T' && vt->c.x == 0);
	/* split UTF-8 across writes */
	vt_write(vt, "\xe2\x82", 2);
	vt_write(vt, "\xac", 1);
	CHECK(vt->pri[2]->c[0].cp == 0x20ac);
	/* invalid UTF-8 shows a replacement char */
	w(vt, "\xff");
	CHECK(vt->pri[2]->c[1].cp == 0xfffd);
	/* unknown sequences are ignored cleanly */
	w(vt, "\033[?1234;5h\033[>4;2m\033[=5uX");
	CHECK(vt->pri[2]->c[2].cp == 'X');
	vt_free(vt);

	/* Overlong UTF-8 is refused: E0 80 9B and F0 80 80 9B would be ESC, and the
	 * [31m after them a color; so would E0 81 81 be an A. The shortest valid
	 * three- and four-byte forms still pass. */
	vt = mk(10, 3, 0);
	w(vt, "\xe0\x80\x9b[31mX");
	CHECK(vt->pri[0]->c[0].cp == 0xfffd && vt->pri[0]->c[1].cp == '[');
	CHECK(vt->pri[0]->c[5].cp == 'X' && vt->pri[0]->c[5].fg == COL_DEFAULT_FG);
	w(vt, "\r\n\xf0\x80\x80\x9b[31mX\xe0\x81\x81");
	CHECK(vt->pri[1]->c[0].cp == 0xfffd && vt->pri[1]->c[1].cp == '[');
	CHECK(vt->pri[1]->c[5].fg == COL_DEFAULT_FG && vt->pri[1]->c[6].cp == 0xfffd);
	w(vt, "\r\n\xe0\xa0\x80\xf0\x90\x80\x80");
	CHECK(vt->pri[2]->c[0].cp == 0x800 && vt->pri[2]->c[1].cp == 0x10000);
	vt_free(vt);

	/* REP writes about a screenful, so a few bytes cannot ask for 65535
	 * cells, but leaves the last row as all 65536 x would: 6 on 10x3; two
	 * lines go into the history instead of about 6550 */
	vt = mk(10, 3, 100);
	w(vt, "x\033[65535b");
	CHECK(vt->hist_count == 2);
	CHECK_STR(row(vt, 1), "xxxxxxxxxx");
	CHECK_STR(row(vt, 2), "xxxxxx");
	vt_free(vt);
}

/* VT100 features mwt does not have, as old animations and vttest send them:
 * they must leave text, cursor and modes as if they had not been sent. */
static void test_unsupported_vt100(void)
{
	struct vt *vt = mk(10, 3, 0);
	/* double height/width lines, single width: text stays normal */
	w(vt, "\033#3ab\033#4cd\033#6ef\033#5gh");
	CHECK_STR(row(vt, 0), "abcdefgh");
	CHECK(vt->c.x == 8 && vt->c.y == 0);
	/* 132 columns, reverse screen, smooth scroll, interlace: no change */
	unsigned mode = vt->mode;
	w(vt, "\033[?3h\033[?5h\033[?4h\033[?9h\033[?3l\033[?5l");
	CHECK(vt->cols == 10 && vt->mode == mode && vt->c.x == 8);
	/* blink is no attribute: the cell is as one written without it */
	w(vt, "\r\n\033[5;6mB\033[25m\033[m");
	CHECK(vt->pri[1]->c[0].cp == 'B' && vt->pri[1]->c[0].attr == ATTR_TEXT);
	/* DECREQTPARM, LEDs, ENQ, DECID: no answer, nothing written */
	replybuf[0] = 0;
	w(vt, "\033[x\033[1x\033[1;2q\005\033ZC");
	CHECK_STR(replybuf, "");
	CHECK_STR(row(vt, 1), "BC");
	/* a national set (UK) and G2/G3 shifts are plain ASCII */
	w(vt, "\033(A#\033)A\016#\017\033*0\033nx\033o\033Ny");
	CHECK_STR(row(vt, 1), "BC##xy");
	CHECK(vt->c.charset[0] == 0 && vt->c.charset[1] == 0 && vt->c.gl == 0);
	/* VT52 mode is not entered: ESC A afterwards is not "cursor up" */
	w(vt, "\033[?2l\033Az");
	CHECK(vt->c.y == 1 && vt->pri[1]->c[6].cp == 'z');
	/* nor newline mode: a line feed stays in its column */
	w(vt, "\033[20h\033[1;3H\nL");
	CHECK(vt->c.y == 1 && vt->c.x == 3 && vt->pri[1]->c[2].cp == 'L');
	/* the alignment pattern (DECALN) is not drawn */
	w(vt, "\033#8");
	CHECK_STR(row(vt, 0), "abcdefgh");
	CHECK_STR(row(vt, 2), "");
	vt_free(vt);
}

static void test_resize(void)
{
	struct vt *vt = mk(10, 5, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5");
	vt_resize(vt, 6, 3);
	CHECK_STR(row(vt, 0), "3");
	CHECK_STR(row(vt, 2), "5");
	CHECK(vt->c.y == 2 && vt->hist_count == 2);
	/* taller again, the lines pushed off come back, and the cursor with them */
	vt_resize(vt, 12, 6);
	CHECK_STR(row(vt, 0), "1");
	CHECK_STR(row(vt, 4), "5");
	CHECK_STR(row(vt, 5), "");
	CHECK(vt->c.y == 4 && vt->hist_count == 0 && vt->scrolled == 0);
	w(vt, "\033[6;12Hx");
	CHECK(vt->pri[5]->c[11].cp == 'x');
	vt_free(vt);

	/* Behind the alternate screen the main one keeps the row its cursor was in
	 * when that one took over, also with 47 and 1047, which save no cursor:
	 * going by saved_pri, the top row then, it lost its last lines instead. */
	static const char *const enter[] = { "\033[?47h", "\033[?1047h", "\033[?1049h" };
	static const char *const leave[] = { "\033[?47l", "\033[?1047l", "\033[?1049l" };
	for (int i = 0; i < 3; i++) {
		vt = mk(10, 6, 10);
		w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6");
		w(vt, enter[i]);
		w(vt, "\033[H"); /* the alternate screen's own cursor asks for no shift */
		vt_resize(vt, 10, 4);
		CHECK(vt->hist_count == 2);
		/* again: the row counts from where the first resize left it */
		vt_resize(vt, 10, 2);
		CHECK(vt->hist_count == 4);
		CHECK_STR(hist_line(vt, 1), "4");
		CHECK_STR(hist_line(vt, 4), "1");
		w(vt, leave[i]);
		CHECK_STR(row(vt, 0), "5");
		CHECK_STR(row(vt, 1), "6");
		/* 1049 brings the cursor back to its line, 47 and 1047 leave it be */
		CHECK(vt->c.y == (i == 2 ? 1 : 0));
		vt_free(vt);
	}

	/* a cursor DECSC saved further down than the one left behind moves with
	 * the screen and stays on it */
	vt = mk(10, 6, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\0337\033[4;1H\033[?47h\033[H");
	vt_resize(vt, 10, 3);
	CHECK(vt->hist_count == 1 && vt->saved_pri.y == 2);
	w(vt, "\033[?47l");
	CHECK_STR(row(vt, 0), "2");
	CHECK_STR(row(vt, 2), "4");
	vt_free(vt);

	/* and one saved above it moves up with its line, into the top row when
	 * that line went into the history */
	vt = mk(10, 6, 10);
	w(vt, "1\r\n2\r\n3\0337\r\n4\r\n5\r\n6\033[?47h\033[H");
	vt_resize(vt, 10, 5);
	CHECK(vt->hist_count == 1 && vt->saved_pri.y == 1);
	vt_resize(vt, 10, 2);
	CHECK(vt->hist_count == 4 && vt->saved_pri.y == 0);
	vt_free(vt);

	/* the other way round: the alternate screen behind the main one keeps
	 * the row of the cursor it saved, which moves with it once */
	vt = mk(10, 6, 10);
	w(vt, "\033[?47h\033[6;1Hx\0337\033[?47l");
	vt_resize(vt, 10, 4);
	CHECK(vt->saved_alt.y == 3 && vt->alt[3]->c[0].cp == 'x');
	w(vt, "\033[?47h\0338");
	CHECK(vt->c.y == 3 && vt->c.x == 1);
	vt_free(vt);

	/* each screen by its own cursor: the alternate one's at the top keeps its
	 * rows while the main one's at the bottom gives two to the history */
	vt = mk(10, 6, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\033[?1049h\033[Ha");
	vt_resize(vt, 10, 4);
	CHECK_STR(row(vt, 0), "a");
	CHECK(vt->c.y == 0 && vt->hist_count == 2);
	w(vt, "\033[?1049l");
	CHECK_STR(row(vt, 0), "3");
	CHECK_STR(row(vt, 3), "6");
	CHECK(vt->c.y == 3);
	vt_free(vt);
	/* and the alternate one behind, by the cursor it saved */
	vt = mk(10, 6, 10);
	w(vt, "\033[?47h\033[2;1Hx\0337\033[?47l\033[6;1H");
	vt_resize(vt, 10, 4);
	CHECK(vt->saved_alt.y == 1 && vt->alt[1]->c[0].cp == 'x' && vt->hist_count == 2);
	vt_free(vt);
	/* shown, its saved cursor moves up with its rows */
	vt = mk(10, 6, 10);
	w(vt, "\033[?47h\033[4;1Hy\0337\033[6;1H");
	vt_resize(vt, 10, 4);
	CHECK(vt->saved_alt.y == 1 && vt->c.y == 3);
	w(vt, "\0338");
	CHECK(vt->c.y == 1 && vt->c.x == 1);
	vt_free(vt);

	/* fewer columns alone move nothing up; a history too small, or none,
	 * takes what it can and the rows are the same */
	vt = mk(10, 6, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\033[?47h\033[H");
	vt_resize(vt, 5, 6);
	CHECK(vt->hist_count == 0 && vt->pri_y == 5);
	vt_free(vt);
	for (int cap = 0; cap < 2; cap++) {
		vt = mk(10, 6, cap);
		w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\033[?47h\033[H");
		vt_resize(vt, 10, 2);
		CHECK(vt->hist_count == cap && vt->pri_y == 1);
		if (cap)
			CHECK_STR(hist_line(vt, 1), "4");
		w(vt, "\033[?47l");
		CHECK_STR(row(vt, 0), "5");
		CHECK_STR(row(vt, 1), "6");
		vt_free(vt);
	}
}

/* A taller window takes the newest history lines back onto the main screen, as
 * xterm does, so what a shorter one pushed off returns with it. */
static void test_resize_back(void)
{
	/* no more than the rows it grew by, and the history goes on after them: a
	 * ring that has gone round, its head taken back past the start */
	struct vt *vt = mk(10, 3, 4);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9");
	CHECK(vt->hist_count == 4 && vt->scrolled == 6);
	CHECK(vt_resize(vt, 10, 5));
	CHECK_STR(row(vt, 0), "5");
	CHECK_STR(row(vt, 1), "6");
	CHECK_STR(row(vt, 2), "7");
	CHECK_STR(row(vt, 4), "9");
	CHECK(vt->c.y == 4 && vt->c.x == 1);
	CHECK(vt->hist_count == 2 && vt->scrolled == 4 && vt_abs_first(vt) == 2);
	CHECK_STR(hist_line(vt, 1), "4");
	CHECK_STR(hist_line(vt, 2), "3");
	w(vt, "\r\na\r\nb\r\nc");
	CHECK(vt->hist_count == 4 && vt->scrolled == 7);
	CHECK_STR(hist_line(vt, 1), "7");
	CHECK_STR(hist_line(vt, 4), "4");
	CHECK_STR(row(vt, 4), "c");
	vt_free(vt);

	/* more rows than the history has lines: blank ones below, as before */
	vt = mk(10, 2, 10);
	w(vt, "1\r\n2\r\n3");
	CHECK(vt_resize(vt, 10, 6));
	CHECK_STR(row(vt, 0), "1");
	CHECK_STR(row(vt, 2), "3");
	CHECK_STR(row(vt, 3), "");
	CHECK(vt->c.y == 2 && vt->hist_count == 0 && vt->scrolled == 0);
	vt_free(vt);

	/* without a history there is nothing to take */
	vt = mk(10, 2, 0);
	w(vt, "1\r\n2\r\n3");
	CHECK(vt_resize(vt, 10, 4));
	CHECK_STR(row(vt, 0), "2");
	CHECK_STR(row(vt, 1), "3");
	CHECK(vt->c.y == 1 && vt->scrolled == 0);
	vt_free(vt);

	/* a line that goes on in the next row still does on the screen */
	vt = mk(5, 2, 10);
	w(vt, "abcdefgh\r\nX");
	CHECK(vt->hist_count == 1 && vt_abs_wraps(vt, vt_abs_first(vt)) == 5);
	CHECK(vt_resize(vt, 5, 3));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK_STR(row(vt, 1), "fgh");
	CHECK(vt->pri[0]->wrap == 5 && vt->pri[0]->used == 5 && vt_abs_wraps(vt, 0) == 5);
	CHECK(vt->pri[1]->wrap == 0);
	vt_free(vt);

	/* Behind the alternate screen too, which gets none itself: the main
	 * screen's cursor row and its saved cursor move down with the lines, so a
	 * shorter window afterwards pushes the same ones off again. */
	static const char *const enter[] = { "\033[?47h", "\033[?1049h" };
	static const char *const leave[] = { "\033[?47l", "\033[?1049l" };
	for (int i = 0; i < 2; i++) {
		vt = mk(10, 2, 10);
		w(vt, "1\r\n2\r\n3\r\n4");
		w(vt, enter[i]);
		w(vt, "\033[HALT");
		CHECK(vt_resize(vt, 10, 4));
		CHECK_STR(row(vt, 0), "ALT");
		CHECK_STR(row(vt, 1), "");
		CHECK(vt->c.y == 0 && vt->hist_count == 0 && vt->pri_y == 3);
		CHECK(vt_resize(vt, 10, 2));
		CHECK(vt->hist_count == 2 && vt->pri_y == 1);
		CHECK(vt_resize(vt, 10, 4));
		w(vt, leave[i]);
		CHECK_STR(row(vt, 0), "1");
		CHECK_STR(row(vt, 3), "4");
		CHECK(vt->hist_count == 0 && vt->c.y == (i ? 3 : 0));
		vt_free(vt);
	}

	/* Shorter and taller again leaves a full screen and its history as they
	 * were, cell for cell: lines that wrap, wide glyphs and colours, against a
	 * second terminal that was never resized. */
	fuzz_state = 0x9e3779b97f4a7c15ULL;
	int bad = 0;
	for (int round = 0; round < 300; round++) {
		int cols = 2 + (int)(fuzz_rand() % 30), rows = 2 + (int)(fuzz_rand() % 8);
		struct vt *a = mk(cols, rows, 256), *b = mk(cols, rows, 256);
		/* at least a screenful, so the cursor ends in the last row */
		for (int i = rows + (int)(fuzz_rand() % 20); i > 0; i--) {
			char s[2 * 31 * 5 + 3]; /* 2 * cols pieces of 5 bytes at most */
			int n = 0;
			for (int k = (int)(fuzz_rand() % (unsigned)(2 * cols)); k > 0; k--) {
				switch (fuzz_rand() % 12) {
				case 0: n += sprintf(s + n, "\xe6\xbc\xa2"); break;
				case 1: n += sprintf(s + n, "\033[%um", 40 + fuzz_rand() % 8); break;
				case 2: n += sprintf(s + n, "\033[%cm", fuzz_rand() % 2 ? '4' : '0'); break;
				default: s[n++] = (char)('a' + fuzz_rand() % 26);
				}
			}
			memcpy(s + n, "\r\n", 3);
			w(a, s);
			w(b, s);
		}
		int k = 1 + (int)(fuzz_rand() % (unsigned)(rows - 1));
		if (!vt_resize(b, cols, rows - k) || !vt_resize(b, cols, rows)) {
			bad++;
		} else {
			bad += a->c.x != b->c.x || a->c.y != b->c.y || a->hist_count != b->hist_count ||
			       a->scrolled != b->scrolled;
			for (int r = 0; r < rows; r++)
				bad += !fuzz_rows(a->pri[r], b->pri[r], cols);
			for (int64_t i = vt_abs_first(a); i < (int64_t)a->scrolled; i++) {
				int la, lb;
				const struct cell *ca = vt_abs_line(a, i, &la), *cb = vt_abs_line(b, i, &lb);
				bad += !ca || !cb || la != lb || !fuzz_cells(ca, cb, la) ||
				       vt_abs_wraps(a, i) != vt_abs_wraps(b, i);
			}
		}
		vt_free(a);
		vt_free(b);
	}
	CHECK(bad == 0);
}

/* The text of the history and the main screen, line by line as a selection
 * would join the rows, with each cell's colours and attributes, and behind each
 * line the colour its end was erased in: what a reflow must leave as it was.
 * Blank lines at the end do not count. */
static size_t reflow_text(const struct vt *vt, uint32_t *out, size_t cap)
{
	size_t n = 0, text = 0;
	bool any = false;
	for (int64_t a = vt_abs_first(vt); a < (int64_t)vt->scrolled + vt->rows; a++) {
		int len, wraps = vt_abs_wraps(vt, a);
		const struct cell *l = vt_abs_line(vt, a, &len);
		uint32_t fill;
		if (a >= (int64_t)vt->scrolled) {
			/* the main screen's rows, also behind the alternate screen,
			 * by the rule of vt_abs_wraps */
			const struct row *r = vt->pri[a - (int64_t)vt->scrolled];
			l = r->c;
			len = vt->cols;
			wraps = r->wrap;
			fill = r->fill;
			for (int k = wraps; wraps && k < len; k++)
				if ((l[k].cp != ' ' && l[k].cp != 0) || l[k].bg != COL_DEFAULT_BG ||
				    (l[k].attr & ~ATTR_TEXT))
					wraps = 0;
		} else {
			fill = vt_abs_fill(vt, a);
		}
		/* blanks at the end are no text: those nothing shows of, or those a
		 * row was erased with up to the last column, which are put there
		 * again; with none of these left in a full row, the row has the
		 * colour */
		if (wraps) {
			len = wraps;
			fill = COL_DEFAULT_BG;
		} else if (fill != COL_DEFAULT_BG) {
			;
		} else if (len == vt->cols && !(l[len - 1].attr & ATTR_TEXT) &&
			   l[len - 1].bg != COL_DEFAULT_BG) {
			fill = l[len - 1].bg;
			while (len > 0 && !(l[len - 1].attr & ATTR_TEXT) && l[len - 1].bg == fill)
				len--;
		} else {
			while (len > 0 && l[len - 1].cp == ' ' && l[len - 1].bg == COL_DEFAULT_BG &&
			       !(l[len - 1].attr & ~ATTR_TEXT))
				len--;
		}
		for (int x = 0; x < len && n + 6 < cap; x++) {
			if (l[x].attr & ATTR_WDUMMY)
				continue;
			out[n++] = l[x].cp;
			out[n++] = l[x].bg;
			out[n++] = l[x].attr & ~ATTR_TEXT; /* a reflow sets it on what is text */
			any = true;
		}
		if (!wraps && n + 2 < cap) {
			out[n++] = '\n';
			out[n++] = fill;
			if (any)
				text = n;
			any = false;
		}
	}
	return text;
}

/* A line of random text for the reflow tests, ended by CR LF: letters, blanks
 * within it, wide glyphs, colours and underlines. No blank at its end: one
 * there is no text to the terminal, which a reflow would not keep. With bg the
 * colours are backgrounds, and rows scrolled in are erased with them up to
 * their last column; without, foregrounds, which erase nothing. */
static void reflow_line(char *s, int cols, bool bg)
{
	int n = 0;
	for (int k = (int)(fuzz_rand() % (unsigned)(2 * cols)); k > 0; k--) {
		switch (fuzz_rand() % 12) {
		case 0: n += sprintf(s + n, "\xe6\xbc\xa2"); break;
		case 1: n += sprintf(s + n, "\033[%um", (bg ? 40 : 30) + fuzz_rand() % 8); break;
		case 2: n += sprintf(s + n, "\033[%cm", fuzz_rand() % 2 ? '4' : '0'); break;
		case 3: s[n++] = ' '; break;
		default: s[n++] = (char)('a' + fuzz_rand() % 26);
		}
	}
	strcpy(s + n, n ? "z\r\n" : "\r\n");
}
#define REFLOW_LINE (2 * 31 * 5 + 4) /* 2 * cols pieces of 5 bytes at most */

/* A window of another width lays the lines of the main screen and the history
 * out anew: rows that went on in the next are one line, wrapped where the new
 * width ends. */
static void test_reflow(void)
{
	int len;
	/* wider and narrower, the cursor behind its text */
	struct vt *vt = mk(10, 4, 10);
	w(vt, "0123456789abcdefghijklmno");
	CHECK(vt_resize(vt, 20, 4));
	CHECK_STR(row(vt, 0), "0123456789abcdefghij");
	CHECK_STR(row(vt, 1), "klmno");
	CHECK_STR(row(vt, 2), "");
	CHECK(vt_abs_wraps(vt, 0) == 20 && vt_abs_wraps(vt, 1) == 0);
	CHECK(vt->c.x == 5 && vt->c.y == 1 && vt->hist_count == 0);
	CHECK(vt_resize(vt, 7, 4));
	CHECK_STR(row(vt, 0), "0123456");
	CHECK_STR(row(vt, 3), "lmno");
	CHECK(vt_abs_wraps(vt, 2) == 7 && vt->pri[3]->used == 4);
	CHECK(vt->c.x == 4 && vt->c.y == 3);
	CHECK(vt_resize(vt, 10, 4));
	CHECK_STR(row(vt, 0), "0123456789");
	CHECK_STR(row(vt, 2), "klmno");
	CHECK(vt->c.x == 5 && vt->c.y == 2 && vt->hist_count == 0);
	vt_free(vt);

	/* A cursor in the last row stays there: narrower, what no longer fits above
	 * it goes into the history, row by row of the new width, and wider it
	 * comes back, a line that began in the history as one. */
	vt = mk(8, 3, 10);
	w(vt, "abcdefgh12\r\nxy\r\nz");
	CHECK(vt->hist_count == 1);
	CHECK(vt_resize(vt, 4, 3));
	CHECK(vt->hist_count == 2 && vt->scrolled == 2 && vt->c.x == 1 && vt->c.y == 2);
	CHECK_STR(hist_line(vt, 2), "abcd");
	CHECK_STR(hist_line(vt, 1), "efgh");
	CHECK(vt_abs_wraps(vt, 0) == 4 && vt_abs_wraps(vt, 1) == 4);
	CHECK_STR(row(vt, 0), "12");
	CHECK_STR(row(vt, 2), "z");
	CHECK(vt_resize(vt, 8, 3));
	CHECK(vt->hist_count == 1 && vt->scrolled == 1 && vt->c.x == 1 && vt->c.y == 2);
	CHECK_STR(hist_line(vt, 1), "abcdefgh");
	CHECK_STR(row(vt, 0), "12");
	CHECK(vt_resize(vt, 16, 3));
	CHECK(vt->hist_count == 0 && vt->c.x == 1 && vt->c.y == 2);
	CHECK_STR(row(vt, 0), "abcdefgh12");
	CHECK_STR(row(vt, 1), "xy");
	vt_free(vt);

	/* One further up moves with its text, and nothing comes back from the
	 * history: what was cleared stays so. A taller window takes lines back as
	 * it does without another width. */
	vt = mk(10, 4, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\033[2J\033[HabcdefghijKL");
	CHECK(vt->hist_count == 2 && vt->c.y == 1);
	CHECK(vt_resize(vt, 20, 4));
	CHECK_STR(row(vt, 0), "abcdefghijKL");
	CHECK_STR(row(vt, 1), "");
	CHECK(vt->hist_count == 2 && vt->c.x == 12 && vt->c.y == 0);
	CHECK(vt_resize(vt, 5, 4));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK_STR(row(vt, 2), "KL");
	CHECK(vt->hist_count == 2 && vt->c.x == 2 && vt->c.y == 2);
	CHECK(vt_resize(vt, 7, 5));
	CHECK_STR(row(vt, 0), "2");
	CHECK_STR(row(vt, 1), "abcdefg");
	CHECK_STR(row(vt, 2), "hijKL");
	CHECK(vt->hist_count == 1 && vt->c.x == 5 && vt->c.y == 2);
	vt_free(vt);

	/* what no longer fits below the cursor is lost, as in a shorter window */
	vt = mk(10, 3, 10);
	w(vt, "0123456789abcde\033[1;4H");
	CHECK(vt_resize(vt, 4, 3));
	CHECK_STR(row(vt, 0), "0123");
	CHECK_STR(row(vt, 2), "89ab");
	CHECK(vt->c.x == 3 && vt->c.y == 0 && vt->hist_count == 0);
	CHECK(vt_resize(vt, 10, 3));
	CHECK_STR(row(vt, 0), "0123456789");
	CHECK_STR(row(vt, 1), "ab");
	vt_free(vt);

	/* A wrap that was pending is the place behind the text: in a wider row the
	 * cursor is there, and the next character goes beside the last. */
	vt = mk(10, 3, 10);
	w(vt, "0123456789");
	CHECK(vt->c.x == 9 && vt->c.wrapnext);
	CHECK(vt_resize(vt, 20, 3));
	CHECK(vt->c.x == 10 && vt->c.y == 0 && !vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "0123456789X");
	vt_free(vt);
	/* and a cursor behind text that now fills its row waits there to wrap,
	 * rather than write over the last character; not without autowrap */
	vt = mk(20, 3, 10);
	w(vt, "0123456789");
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->c.x == 9 && vt->c.y == 0 && vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "0123456789");
	CHECK_STR(row(vt, 1), "X");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	vt_free(vt);
	vt = mk(20, 3, 10);
	w(vt, "\033[?7l0123456789");
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->c.x == 9 && !vt->c.wrapnext);
	vt_free(vt);
	/* a cursor beyond the text keeps its distance from it, up to the last
	 * column, and with no wrap pending there */
	vt = mk(20, 2, 10);
	w(vt, "$ ");
	CHECK(vt_resize(vt, 10, 2));
	CHECK(vt->c.x == 2 && vt->c.y == 0);
	vt_free(vt);
	vt = mk(20, 2, 10);
	w(vt, "ab\033[1;18H");
	CHECK(vt_resize(vt, 12, 2));
	CHECK(vt->c.x == 5 && vt->c.y == 1 && !vt->c.wrapnext && vt->pri[0]->wrap == 12);
	CHECK(vt_resize(vt, 30, 2));
	CHECK(vt->c.x == 17 && vt->c.y == 0 && vt->pri[0]->wrap == 0);
	vt_free(vt);
	/* on a cell of the text, it stays on that cell */
	vt = mk(10, 4, 10);
	w(vt, "0123456789abcde\033[2;3H");
	CHECK(vt->pri[1]->c[2].cp == 'c');
	CHECK(vt_resize(vt, 4, 4));
	CHECK(vt->c.y == 3 && vt->c.x == 0 && vt->pri[3]->c[0].cp == 'c');
	CHECK(vt_resize(vt, 30, 4));
	CHECK(vt->c.y == 0 && vt->c.x == 12 && vt->pri[0]->c[12].cp == 'c');
	vt_free(vt);

	/* on the cell a wide glyph did not fit in, which is no text and gone in
	 * another width, it is on the last cell before it */
	vt = mk(10, 3, 10);
	w(vt, "abcdefghi\xe6\xbc\xa2X\033[1;10H");
	CHECK(vt->c.x == 9 && vt->c.y == 0 && vt->pri[0]->wrap == 9);
	CHECK(vt_resize(vt, 20, 3));
	CHECK(vt->c.x == 8 && vt->c.y == 0 && vt->pri[0]->c[8].cp == 'i');
	CHECK(vt_resize(vt, 6, 3));
	CHECK(vt->c.x == 2 && vt->c.y == 1 && vt->pri[1]->c[2].cp == 'i');
	vt_free(vt);

	/* Wide glyphs stay whole: one that does not fit goes to the next row, and
	 * the cell left over is not the line's. A window one column wide has no
	 * room for them, as it has none when they are written. */
	vt = mk(9, 4, 10);
	w(vt, "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"
	      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9eX");
	CHECK(vt->pri[0]->wrap == 8 && vt->pri[1]->wrap == 8 && vt->pri[2]->used == 3);
	CHECK(vt_resize(vt, 8, 4));
	CHECK(vt->pri[0]->wrap == 8 && vt->pri[0]->used == 8 && vt->pri[2]->used == 3);
	CHECK(vt_resize(vt, 7, 4));
	CHECK(vt->pri[0]->wrap == 6 && vt->pri[0]->used == 6 && vt->pri[0]->c[6].cp == ' ');
	CHECK((vt->pri[0]->c[4].attr & ATTR_WIDE) && (vt->pri[0]->c[5].attr & ATTR_WDUMMY));
	CHECK(vt->pri[2]->wrap == 0 && vt->pri[2]->c[6].cp == 'X');
	CHECK(vt->c.y == 2 && vt->c.x == 6 && vt->c.wrapnext);
	CHECK(vt_resize(vt, 19, 4));
	CHECK(vt->pri[0]->used == 19 && vt->pri[0]->wrap == 0 && vt->pri[0]->c[18].cp == 'X');
	CHECK(vt_resize(vt, 1, 4));
	CHECK_STR(row(vt, 0), "X");
	CHECK(vt->c.x == 0 && vt->c.y == 0 && vt->c.wrapnext);
	vt_free(vt);

	/* colours and attributes go with their cells, an empty line stays one */
	vt = mk(6, 4, 10);
	w(vt, "ab\033[41;4mcdefgh\033[m\r\n\r\nx");
	CHECK(vt_resize(vt, 3, 6));
	CHECK_STR(row(vt, 0), "abc");
	CHECK_STR(row(vt, 2), "gh");
	CHECK_STR(row(vt, 3), "");
	CHECK_STR(row(vt, 4), "x");
	CHECK(vt->pri[0]->c[1].bg == COL_DEFAULT_BG && vt->pri[0]->c[2].bg == 1 &&
	      (vt->pri[2]->c[1].attr & ATTR_UNDERLINE) && vt->pri[2]->c[1].bg == 1);
	vt_free(vt);

	/* a screen that was cleared does not go on from the history line above it,
	 * and one that was not does */
	vt = mk(10, 2, 10);
	w(vt, "0123456789abcdefghijKLM");
	CHECK(vt_resize(vt, 30, 2));
	CHECK_STR(row(vt, 0), "0123456789abcdefghijKLM");
	CHECK(vt->hist_count == 0 && vt->c.x == 23 && vt->c.y == 0);
	vt_free(vt);
	vt = mk(10, 2, 10);
	w(vt, "0123456789abcdefghijKLM\033[2J\033[Hxyz");
	CHECK(vt_resize(vt, 30, 2));
	CHECK_STR(hist_line(vt, 1), "0123456789");
	CHECK_STR(row(vt, 0), "xyz");
	CHECK(vt->hist_count == 1 && vt->c.x == 3 && vt->c.y == 0);
	vt_free(vt);

	/* The alternate screen is cut, not laid out anew: its program draws it
	 * again. The main screen behind it is, by the row its cursor left, and
	 * 1049 brings the cursor back to its text. */
	vt = mk(10, 3, 10);
	w(vt, "0123456789abcde\033[?1049h\033[HALTSCREEN!\r\nrow two");
	CHECK(vt_resize(vt, 5, 3));
	CHECK_STR(row(vt, 0), "ALTSC");
	CHECK_STR(row(vt, 1), "row t");
	CHECK(vt->alt[0]->wrap == 0 && vt->pri_y == 2 && vt->hist_count == 0);
	w(vt, "\033[?1049l");
	CHECK_STR(row(vt, 0), "01234");
	CHECK_STR(row(vt, 2), "abcde");
	CHECK(vt->c.x == 4 && vt->c.y == 2 && vt->c.wrapnext);
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "0123456789abcdefghijklmnopqrstuvwxyz\033[?47h");
	CHECK(vt->hist_count == 1 && vt->pri_x == 6 && vt->pri_y == 2);
	CHECK(vt_resize(vt, 20, 3));
	CHECK(vt->hist_count == 0 && vt->pri_x == 16 && vt->pri_y == 1);
	CHECK(vt_resize(vt, 4, 3));
	CHECK(vt->hist_count == 6 && vt->pri_x == 4 && vt->pri_y == 2);
	w(vt, "\033[?47l");
	CHECK_STR(hist_line(vt, 6), "0123");
	CHECK_STR(row(vt, 2), "wxyz");
	vt_free(vt);

	/* a history too short for the rows keeps the newest, and the lines count
	 * on past those that fell off; none at all keeps none */
	vt = mk(10, 2, 3);
	w(vt, "0123456789abcdefghijKLMNOPQRST");
	CHECK(vt->hist_count == 1 && vt_abs_first(vt) == 0);
	CHECK(vt_resize(vt, 5, 2));
	CHECK(vt->hist_count == 3 && vt->scrolled == 4 && vt_abs_first(vt) == 1);
	CHECK_STR(hist_line(vt, 3), "56789");
	CHECK_STR(hist_line(vt, 1), "fghij");
	CHECK_STR(row(vt, 0), "KLMNO");
	CHECK(!vt_abs_line(vt, 0, &len));
	w(vt, "\r\nu");
	CHECK(vt->hist_count == 3 && vt->scrolled == 5);
	CHECK_STR(hist_line(vt, 1), "KLMNO");
	CHECK_STR(hist_line(vt, 3), "abcde");
	vt_free(vt);
	vt = mk(10, 2, 0);
	w(vt, "0123456789abcdefghijKL");
	CHECK(vt_resize(vt, 5, 2));
	CHECK_STR(row(vt, 0), "fghij");
	CHECK_STR(row(vt, 1), "KL");
	CHECK(vt->hist_count == 0 && vt->scrolled == 0 && vt->c.x == 2 && vt->c.y == 1);
	vt_free(vt);

	/* Narrower or wider and back leaves a full screen and its history as they
	 * were, cell for cell, against a second terminal that was never resized.
	 * With rows erased in a colour as well: where the text filled a row of the
	 * other width to its end, the row kept the colour. What is text and what
	 * was erased behind it is the same too (reflow_text). */
	enum { TEXT_MAX = 1 << 16 };
	static uint32_t ta[TEXT_MAX], tb[TEXT_MAX];
	fuzz_state = 0x243f6a8885a308d3ULL;
	int bad = 0, filled = 0;
	for (int round = 0; round < 600; round++) {
		bool bg = round % 3 == 2;
		int cols = 2 + (int)(fuzz_rand() % 30), rows = 2 + (int)(fuzz_rand() % 8);
		struct vt *a = mk(cols, rows, 4096), *b = mk(cols, rows, 4096);
		for (int i = rows + (int)(fuzz_rand() % 20); i > 0; i--) {
			char s[REFLOW_LINE];
			reflow_line(s, cols, bg);
			w(a, s);
			w(b, s);
		}
		int other = 2 + (int)(fuzz_rand() % 60);
		if (!vt_resize(b, other, rows) || !vt_resize(b, cols, rows) || fuzz_broken(b)) {
			bad++;
		} else {
			size_t na = reflow_text(a, ta, TEXT_MAX), nb = reflow_text(b, tb, TEXT_MAX);
			bad += same_state(a, b, SAME_BUT_SAVED | SAME_BUT_MARKS) || na != nb ||
			       memcmp(ta, tb, na * sizeof *ta);
			for (int r = 0; r < rows; r++)
				filled += !(a->pri[r]->c[cols - 1].attr & ATTR_TEXT) &&
					  a->pri[r]->c[cols - 1].bg != COL_DEFAULT_BG;
		}
		vt_free(a);
		vt_free(b);
	}
	CHECK(bad == 0);
	CHECK(filled > 100); /* and rows were erased in a colour up to their end */

	/* Through any run of sizes the text stays what it was: every cell, its
	 * colours and where its line ends, with the cursor behind it all, on the
	 * main screen and behind the alternate one. */
	bad = 0;
	for (int round = 0; round < 300; round++) {
		int cols = 2 + (int)(fuzz_rand() % 30), rows = 1 + (int)(fuzz_rand() % 8);
		struct vt *a = mk(cols, rows, 4096), *b = mk(cols, rows, 4096);
		for (int i = (int)(fuzz_rand() % 30); i > 0; i--) {
			char s[REFLOW_LINE];
			reflow_line(s, cols, round % 2);
			w(a, s);
			w(b, s);
		}
		size_t na = reflow_text(a, ta, TEXT_MAX);
		if (round % 3 == 0)
			w(b, "\033[?1049h");
		for (int i = 1 + (int)(fuzz_rand() % 6); i > 0; i--) {
			bool ok = vt_resize(b, 2 + (int)(fuzz_rand() % 70), 1 + (int)(fuzz_rand() % 12));
			size_t nb = reflow_text(b, tb, TEXT_MAX);
			bad += !ok || fuzz_broken(b) || na != nb || memcmp(ta, tb, na * sizeof *ta);
			if (!(b->mode & MODE_ALTSCREEN))
				bad += b->c.x != 0 || b->c.wrapnext;
		}
		vt_free(a);
		vt_free(b);
	}
	CHECK(bad == 0);

	/* A row has what was erased behind it only while it is full and ends its
	 * line, whatever is written after the reflow that left it: random streams
	 * between random sizes, over rows erased in a colour. */
	int fills = 0, kept = 0;
	bad = 0;
	for (int round = 0; round < 1000; round++) {
		struct vt *v = mk(2 + (int)(fuzz_rand() % 20), 1 + (int)(fuzz_rand() % 6), 30);
		for (int step = 0; step < 6; step++) {
			for (int i = (int)(fuzz_rand() % (unsigned)(2 * v->rows)); i > 0; i--) {
				char s[REFLOW_LINE];
				reflow_line(s, v->cols, true);
				w(v, s);
			}
			w(v, "\033[m");
			/* narrow often: a line's text fills its last row more easily */
			bad += !vt_resize(v, 1 + (int)(fuzz_rand() % (step % 2 ? 24 : 8)),
					  1 + (int)(fuzz_rand() % 8)) || fuzz_broken(v);
			int had = 0;
			for (int r = 0; r < v->rows; r++)
				had += v->pri[r]->fill != COL_DEFAULT_BG;
			fills += had;
			char buf[160];
			vt_write(v, buf, fuzz_gen(buf, sizeof buf));
			bad += fuzz_broken(v) != NULL;
			for (int r = 0; had && r < v->rows; r++)
				kept += v->pri[r]->fill != COL_DEFAULT_BG;
		}
		vt_free(v);
	}
	CHECK(bad == 0);
	CHECK(fills > 200 && kept > 50); /* and rows had one, also after the stream */

	/* blanks written at the end of a line are no text: narrower, they take no
	 * rows of their own */
	vt = mk(10, 4, 10);
	w(vt, "ab        \r\ncd");
	CHECK(vt_resize(vt, 4, 4));
	CHECK_STR(row(vt, 0), "ab");
	CHECK_STR(row(vt, 1), "cd");
	vt_free(vt);

	/* the cell a wide glyph did not fit in, written since: text, and the line
	 * ends in its row, the glyph a line of its own */
	vt = mk(10, 4, 10);
	w(vt, "abcdefghi\xe6\xbc\xa2\033[1;10HZ");
	CHECK(vt->pri[0]->wrap == 9 && vt_abs_wraps(vt, 0) == 0);
	CHECK(vt_resize(vt, 20, 4));
	CHECK_STR(row(vt, 0), "abcdefghiZ");
	CHECK_STR(row(vt, 1), "\xe6\xbc\xa2");
	vt_free(vt);

	/* The blanks between the text and the cursor behind them are text, as if
	 * written: where they fill the row, the cursor waits behind them to wrap.
	 * Not those before the saved cursor, which may be anywhere: further behind
	 * the text than the new width goes it is in the last column, with no wrap
	 * pending. */
	vt = mk(10, 3, 10);
	w(vt, "abc\033[2C");
	CHECK(vt_resize(vt, 5, 3));
	CHECK(vt->c.x == 4 && vt->c.y == 0 && vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "abc");
	CHECK_STR(row(vt, 1), "X");
	CHECK(vt_abs_wraps(vt, 0) == 5);
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "abc\033[2C\0337\r\nxy");
	CHECK(vt_resize(vt, 5, 3));
	w(vt, "\0338");
	CHECK(vt->c.x == 4 && vt->c.y == 0 && !vt->c.wrapnext);
	vt_free(vt);

	/* the saved cursor as the cursor: a wrap pending is the place behind its
	 * cell, and right behind a row its text now fills it is pending */
	vt = mk(5, 3, 10);
	w(vt, "abcde\0337\r\nxy");
	CHECK(vt_resize(vt, 8, 3));
	w(vt, "\0338");
	CHECK(vt->c.x == 5 && vt->c.y == 0 && !vt->c.wrapnext);
	vt_free(vt);
	vt = mk(8, 3, 10);
	w(vt, "abcde\0337\r\nxy");
	CHECK(vt_resize(vt, 5, 3));
	w(vt, "\0338");
	CHECK(vt->c.x == 4 && vt->c.y == 0 && vt->c.wrapnext);
	vt_free(vt);
	/* not when its text is no longer on the screen: the nearest row, and
	 * nothing pending there */
	vt = mk(8, 4, 10);
	w(vt, "abcd\0337\r\nb\r\nc\r\nd");
	CHECK(vt_resize(vt, 4, 2));
	CHECK_STR(row(vt, 0), "c");
	w(vt, "\0338");
	CHECK(vt->c.x == 3 && vt->c.y == 0 && !vt->c.wrapnext);
	vt_free(vt);

	/* a history that has lost lines at its far end: the lines count on from
	 * where they did */
	vt = mk(10, 2, 2);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6");
	CHECK(vt->scrolled == 4 && vt_abs_first(vt) == 2);
	CHECK(vt_resize(vt, 5, 2));
	CHECK(vt->scrolled == 4 && vt_abs_first(vt) == 2 && vt->hist_count == 2);
	CHECK_STR(hist_line(vt, 1), "4");
	CHECK_STR(row(vt, 1), "6");
	vt_free(vt);
}

/* Where a line ends, for a reflow: the blanks before the cursor, the blanks a
 * row was erased with, an empty row the line went on into, and a row whose next
 * fell off the screen. */
/* Plain output, written at one size and resized to another, is what it is
 * written at the other size (REFLOW.md, A2): printable characters of one and
 * two cells, attributes and foreground colours, CR LF. Every row of the screen
 * and the history, where each goes on, the cursor and its pending wrap.
 *
 * Left out until the rule is there: the saved cursor (D20), and lines that end
 * in a blank, which are shorter by it after the resize (D1). */
static void test_plain_output(void)
{
	int bad = 0, shown = 0, wide = 0, hist = 0, wrapped = 0, pending = 0;
	for (int round = 0; round < 3000; round++) {
		char s[4096];
		size_t n = 0;
		int lines = (int)(fuzz_rand() % 12);
		for (int l = 0; l < lines; l++) {
			int len = fuzz_rand() % 5 ? (int)(fuzz_rand() % 60) : 0;
			for (int i = 0; i < len; i++) {
				unsigned k = fuzz_rand() % 20;
				if (k == 0)
					n += (size_t)sprintf(s + n, "\xe6\xbc\xa2");
				else if (k == 1)
					n += (size_t)sprintf(s + n, "\033[%um", 1 + fuzz_rand() % 7);
				else if (k == 2)
					n += (size_t)sprintf(s + n, "\033[3%um", fuzz_rand() % 8);
				else
					s[n++] = k == 3 ? ' ' : (char)('a' + fuzz_rand() % 26);
			}
			if (len)
				s[n++] = 'z';
			if (l + 1 < lines || fuzz_rand() % 2)
				n += (size_t)sprintf(s + n, "\r\n");
		}
		int w0 = 2 + (int)(fuzz_rand() % 40), r0 = 1 + (int)(fuzz_rand() % 12);
		int w1 = 2 + (int)(fuzz_rand() % 40), r1 = 1 + (int)(fuzz_rand() % 12);
		if (round % 8 == 0)
			w1 = w0; /* another height alone */
		struct vt *a = mk(w0, r0, 4096), *b = mk(w1, r1, 4096);
		vt_write(a, s, n);
		vt_write(b, s, n);
		const char *why = vt_resize(a, w1, r1) ? same_state(a, b, SAME_BUT_SAVED) : "no memory";
		if (why) {
			bad++;
			if (shown++ < 5)
				fprintf(stderr, "plain output %d: %dx%d to %dx%d: %s\n", round, w0, r0, w1,
					r1, why);
		}
		hist += b->hist_count > 0;
		pending += b->c.wrapnext;
		for (int r = 0; r < r1; r++) {
			wrapped += b->pri[r]->wrap == w1;
			wide += b->pri[r]->wrap == w1 - 1;
		}
		vt_free(a);
		vt_free(b);
	}
	CHECK(bad == 0);
	/* and it had a history, rows that go on, with a cell left over by a wide
	 * glyph too, and a cursor that waits to wrap */
	CHECK(hist > 500 && wrapped > 1000 && wide > 100 && pending > 20);
}

static void test_reflow_ends(void)
{
	/* A prompt ends in a blank with the cursor behind it. Narrower by so much
	 * that the prompt fills the row, the cursor waits behind the blank and not
	 * on it; wider again it is where it was, and what is typed comes after the
	 * blank, not over it. */
	struct vt *vt = mk(20, 2, 10);
	w(vt, "user@host:~$ ");
	CHECK(vt_resize(vt, 13, 2));
	CHECK(vt->c.x == 12 && vt->c.y == 0 && vt->c.wrapnext);
	CHECK(vt_resize(vt, 20, 2));
	CHECK(vt->c.x == 13 && vt->c.y == 0 && !vt->c.wrapnext);
	w(vt, "ls");
	CHECK_STR(row(vt, 0), "user@host:~$ ls");
	vt_free(vt);
	/* narrower still, the blank goes to the next row and the cursor with it */
	vt = mk(20, 2, 10);
	w(vt, "user@host:~$ ");
	CHECK(vt_resize(vt, 12, 2));
	CHECK(vt->c.x == 1 && vt->c.y == 1 && vt->pri[0]->wrap == 12);
	w(vt, "ls");
	CHECK(vt_resize(vt, 20, 2));
	CHECK_STR(row(vt, 0), "user@host:~$ ls");
	CHECK(vt->c.x == 15 && vt->c.y == 0);
	vt_free(vt);
	/* a cursor moved one behind the text: the next character does not land
	 * on the last one */
	vt = mk(20, 4, 10);
	w(vt, "abcdefghij\033[C");
	CHECK(vt_resize(vt, 10, 4));
	CHECK(vt->c.x == 1 && vt->c.y == 1 && vt->pri[0]->wrap == 10);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "abcdefghij");
	CHECK_STR(row(vt, 1), " X");
	vt_free(vt);
	/* a blank written into the last column, a wrap pending behind it: the
	 * blank is text too, and in a narrower row the cursor waits behind it */
	vt = mk(10, 3, 10);
	w(vt, "abcdefghi ");
	CHECK(vt->c.x == 9 && vt->c.wrapnext);
	CHECK(vt_resize(vt, 5, 3));
	CHECK(vt->c.x == 4 && vt->c.y == 1 && vt->c.wrapnext && vt->pri[1]->used == 5);
	w(vt, "X");
	CHECK_STR(row(vt, 1), "fghi");
	CHECK_STR(row(vt, 2), "X");
	vt_free(vt);
	/* behind the alternate screen the same, by the place the cursor left; a
	 * wrap that was pending there is the place behind its cell */
	vt = mk(20, 2, 10);
	w(vt, "user@host:~$ \033[?47h");
	CHECK(vt->pri_x == 13);
	CHECK(vt_resize(vt, 13, 2));
	CHECK(vt->pri_x == 13 && vt->pri_y == 0);
	CHECK(vt_resize(vt, 20, 2));
	CHECK(vt->pri_x == 13 && vt->pri_y == 0);
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "0123456789\033[?47h");
	CHECK(vt->pri_x == 10 && vt->pri_y == 0);
	CHECK(vt_resize(vt, 5, 3));
	CHECK(vt->pri_x == 5 && vt->pri_y == 1);
	vt_free(vt);

	/* Rows erased with a background colour up to the last column: the blanks
	 * are no text and take no row of their own in a narrower window. They are
	 * put behind the text again, up to the new last column, wider too. */
	vt = mk(10, 4, 10);
	w(vt, "\033[44m\033[2J\033[Hab\033[0m\r\ncd\033[44m\033[K\033[0m\r\n$ ");
	CHECK(vt_resize(vt, 9, 4));
	CHECK(vt->hist_count == 0 && vt->c.x == 2 && vt->c.y == 2);
	CHECK_STR(row(vt, 0), "ab");
	CHECK_STR(row(vt, 1), "cd");
	CHECK_STR(row(vt, 2), "$");
	CHECK(vt->pri[0]->c[1].bg == 4 && vt->pri[0]->c[8].bg == 4 && vt->pri[0]->used == 9);
	CHECK(vt->pri[1]->c[1].bg == COL_DEFAULT_BG && vt->pri[1]->c[2].bg == 4 && vt->pri[1]->c[8].bg == 4);
	CHECK(vt->pri[2]->c[1].bg == COL_DEFAULT_BG && vt->pri[2]->c[2].bg == 4 && vt->pri[2]->c[8].bg == 4);
	CHECK(vt->pri[3]->c[0].bg == 4 && vt->pri[3]->c[8].bg == 4 && vt->pri[3]->c[8].cp == ' ');
	CHECK(vt_resize(vt, 12, 4));
	CHECK(vt->hist_count == 0 && vt->c.x == 2 && vt->c.y == 2);
	CHECK(vt->pri[1]->c[2].bg == 4 && vt->pri[1]->c[11].bg == 4 && vt->pri[3]->c[11].bg == 4);
	vt_free(vt);
	/* behind text that takes more rows now they are behind its last row, and
	 * back behind the one row it was; in the history as on the screen */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdefgh\033[K\033[m\r\nx\r\ny\r\nz");
	CHECK(vt->hist_count == 1);
	CHECK(vt_resize(vt, 5, 3));
	CHECK(vt->hist_count == 2);
	CHECK_STR(hist_line(vt, 2), "abcde");
	CHECK_STR(hist_line(vt, 1), "fgh");
	int len;
	const struct cell *l = vt_abs_line(vt, vt_abs_first(vt) + 1, &len);
	CHECK(l && len == 5 && l[2].bg == 1 && l[3].cp == ' ' && l[3].bg == 1 && l[4].bg == 1);
	CHECK(vt_abs_wraps(vt, vt_abs_first(vt)) == 5 && vt_abs_wraps(vt, vt_abs_first(vt) + 1) == 0);
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->hist_count == 1);
	l = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK(l && len == 10 && l[7].cp == 'h' && l[8].cp == ' ' && l[8].bg == 1 && l[9].bg == 1);
	vt_free(vt);
	/* Where the text fills a row of the new width to its end, there is no
	 * cell to keep such a blank in: the row has the colour then (row.fill), and
	 * they are back in a wider window. */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt->pri[0]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 6, 3));
	CHECK_STR(row(vt, 0), "abcdef");
	CHECK_STR(row(vt, 1), "x");
	CHECK(vt->pri[0]->fill == 1 && vt->pri[0]->used == 6 && vt->pri[1]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->pri[0]->c[5].bg == 1 && vt->pri[0]->c[6].bg == 1 && vt->pri[0]->c[9].bg == 1);
	CHECK(vt->pri[0]->c[6].cp == ' ' && !vt->pri[0]->c[6].attr && vt->pri[0]->fill == COL_DEFAULT_BG);
	CHECK(vt->pri[1]->c[1].bg == COL_DEFAULT_BG);
	vt_free(vt);
	/* through more widths that the text fills, on the last row of its line;
	 * into the history with it and back, by another height and another width */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 6, 3) && vt_resize(vt, 3, 3));
	CHECK(vt->pri[0]->wrap == 3 && vt->pri[0]->fill == COL_DEFAULT_BG && vt->pri[1]->fill == 1);
	CHECK(vt_resize(vt, 2, 3));
	CHECK(vt->hist_count == 1 && vt->pri[1]->fill == 1 && vt->pri[1]->wrap == 0);
	CHECK(vt_resize(vt, 2, 1));
	CHECK(vt->hist_count == 3);
	l = vt_abs_line(vt, vt_abs_first(vt) + 2, &len);
	CHECK(l && len == 2 && l[1].cp == 'f' && vt_abs_wraps(vt, vt_abs_first(vt) + 2) == 0);
	CHECK(vt_resize(vt, 2, 4));
	CHECK(vt->hist_count == 0 && vt->pri[2]->fill == 1 && vt->pri[1]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 2, 1) && vt_resize(vt, 4, 1));
	CHECK(vt->hist_count == 2);
	l = vt_abs_line(vt, vt_abs_first(vt) + 1, &len);
	CHECK(l && len == 4 && l[1].cp == 'f' && l[2].cp == ' ' && l[2].bg == 1 && l[3].bg == 1);
	CHECK(vt_resize(vt, 10, 3));
	CHECK_STR(row(vt, 0), "abcdef");
	CHECK(vt->pri[0]->c[6].bg == 1 && vt->pri[0]->c[9].bg == 1 && vt->pri[0]->used == 10);
	vt_free(vt);
	/* The blanks between the text and what was erased behind them are text
	 * then, in the history too, which keeps no other blanks at a line's end. */
	vt = mk(10, 2, 10);
	w(vt, "ab\033[1;6H\033[41m\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 5, 2));
	CHECK(vt->pri[0]->fill == 1 && vt->pri[0]->used == 5);
	w(vt, "\r\ny");
	l = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK(vt->hist_count == 1 && l && len == 5);
	CHECK(vt_resize(vt, 10, 2));
	l = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK(l && len == 10 && l[2].bg == COL_DEFAULT_BG && l[4].bg == COL_DEFAULT_BG &&
	      l[5].bg == 1 && l[9].bg == 1);
	vt_free(vt);
	/* The colour is what was right of the row's end: gone once the last cell
	 * is written, as in a terminal this wide, and not with another one. */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 6, 3));
	w(vt, "\033[1;1HA\033[1;5HE");
	CHECK(vt->pri[0]->fill == 1);
	w(vt, "F");
	CHECK(vt->pri[0]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 10, 3));
	CHECK_STR(row(vt, 0), "AbcdEF");
	CHECK(vt->pri[0]->c[6].bg == COL_DEFAULT_BG && vt->pri[0]->used == 6);
	vt_free(vt);
	/* erased, the last cell is what the erase left: nothing, or another colour */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 6, 3));
	w(vt, "\033[1;6H\033[K");
	CHECK(vt->pri[0]->fill == COL_DEFAULT_BG && vt->pri[0]->used == 5);
	CHECK(vt_resize(vt, 10, 3));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK(vt->pri[0]->c[5].bg == COL_DEFAULT_BG && vt->pri[0]->used == 5);
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 6, 3));
	w(vt, "\033[1;6H\033[44m\033[X\033[m");
	CHECK(vt->pri[0]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->pri[0]->c[4].bg == 1 && vt->pri[0]->c[5].bg == 4 && vt->pri[0]->c[9].bg == 4);
	vt_free(vt);
	/* cells put in or taken out move the last one: ICH, insert mode, DCH */
	static const char *const moved[] = { "\033[1;2H\033[@", "\033[1;2H\033[4hX\033[4l", "\033[1;2H\033[P" };
	for (int i = 0; i < 3; i++) {
		vt = mk(10, 3, 10);
		w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
		CHECK(vt_resize(vt, 6, 3));
		CHECK(vt->pri[0]->fill == 1);
		w(vt, moved[i]);
		CHECK(vt->pri[0]->fill == COL_DEFAULT_BG);
		CHECK(vt_resize(vt, 10, 3));
		CHECK(vt->pri[0]->c[9].bg == COL_DEFAULT_BG && vt->pri[1]->c[0].cp == 'x');
		vt_free(vt);
	}
	/* and a line that goes on does not end in the row: the cursor waited
	 * behind the text that fills it, and the next character wraps */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\033[1;7H");
	CHECK(vt_resize(vt, 6, 3));
	CHECK(vt->pri[0]->fill == 1 && vt->c.x == 5 && vt->c.wrapnext);
	w(vt, "g");
	CHECK(vt->pri[0]->wrap == 6 && vt->pri[0]->fill == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 10, 3));
	CHECK_STR(row(vt, 0), "abcdefg");
	CHECK(vt->pri[0]->c[7].bg == COL_DEFAULT_BG);
	vt_free(vt);
	/* so with a wide glyph that leaves the last cell out of the line */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mab\033[m   \033[41m\033[K\033[m\033[1;6H");
	CHECK(vt_resize(vt, 5, 3));
	CHECK(vt->pri[0]->fill == 1 && vt->c.x == 4 && vt->c.wrapnext);
	w(vt, "\033[1;5H\xe6\xbc\xa2");
	CHECK(vt->pri[0]->wrap == 4 && vt->pri[0]->fill == COL_DEFAULT_BG);
	vt_free(vt);
	/* a window of the same width keeps it, one of another height as well; the
	 * alternate screen, which is not laid out anew, has none */
	vt = mk(10, 3, 10);
	w(vt, "\033[41mabcdef\033[K\033[m\r\nx");
	CHECK(vt_resize(vt, 6, 3) && vt_resize(vt, 6, 5));
	CHECK(vt->pri[0]->fill == 1);
	w(vt, "\033[?1049h");
	CHECK(vt_resize(vt, 6, 2) && vt_resize(vt, 10, 2));
	CHECK(vt->alt[0]->fill == COL_DEFAULT_BG && vt->alt[1]->fill == COL_DEFAULT_BG);
	w(vt, "\033[?1049l");
	CHECK(vt->pri[0]->c[5].cp == 'f' && vt->pri[0]->c[6].bg == 1 && vt->pri[0]->c[9].bg == 1);
	vt_free(vt);

	/* Blanks written in a background colour are text, those of an erase are
	 * not (ATTR_TEXT): a bar of written blanks takes the rows its width needs,
	 * up to the last column too, and is as wide as it was in a wider window. */
	vt = mk(10, 4, 10);
	w(vt, "\033[44m          \033[m\r\nX");
	CHECK(vt->pri[0]->used == 10 && (vt->pri[0]->c[9].attr & ATTR_TEXT));
	CHECK(vt_resize(vt, 5, 4));
	CHECK(vt->pri[0]->wrap == 5 && vt->pri[1]->used == 5 && vt->pri[1]->c[4].bg == 4);
	CHECK(vt->pri[1]->fill == COL_DEFAULT_BG);
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt_resize(vt, 12, 4));
	CHECK(vt->pri[0]->c[9].bg == 4 && vt->pri[0]->c[10].bg == COL_DEFAULT_BG && vt->pri[0]->used == 10);
	CHECK_STR(row(vt, 1), "X");
	vt_free(vt);
	/* the row erased in the colour is one row, and reaches the new last column */
	vt = mk(10, 4, 10);
	w(vt, "\033[44m\033[K\033[m\r\nX");
	CHECK(vt->pri[0]->used == 10 && !vt->pri[0]->c[9].attr);
	CHECK(vt_resize(vt, 5, 4));
	CHECK(vt->pri[0]->wrap == 0 && vt->pri[0]->c[4].bg == 4);
	CHECK_STR(row(vt, 1), "X");
	CHECK(vt_resize(vt, 12, 4));
	CHECK(vt->pri[0]->c[11].bg == 4);
	vt_free(vt);
	/* written ones before the erased: text, and what follows them is put
	 * behind them again; erased ones before written ones are in the line */
	vt = mk(10, 4, 10);
	w(vt, "\033[44mab  \033[K\033[m\r\nX");
	CHECK(vt_resize(vt, 4, 4));
	CHECK(vt->pri[0]->used == 4 && vt->pri[0]->wrap == 0 && vt->pri[0]->fill == 4);
	CHECK_STR(row(vt, 1), "X");
	CHECK(vt_resize(vt, 3, 4));
	CHECK(vt->pri[0]->wrap == 3 && vt->pri[1]->used == 3 && vt->pri[1]->c[0].attr == ATTR_TEXT &&
	      !vt->pri[1]->c[1].attr && vt->pri[1]->c[2].bg == 4);
	vt_free(vt);
	vt = mk(10, 4, 10);
	w(vt, "\033[44m\033[K\033[1;9H  \033[m\r\nX");
	CHECK(vt_resize(vt, 5, 4));
	CHECK(vt->pri[0]->wrap == 5 && vt->pri[1]->used == 5 && vt->pri[1]->wrap == 0);
	CHECK(vt->pri[1]->c[2].bg == 4 && vt->pri[1]->c[3].attr == ATTR_TEXT);
	CHECK_STR(row(vt, 2), "X");
	vt_free(vt);
	/* Blanks of an erase that ends before the last column are text, and stay
	 * so where another width puts them in a last column: laid out as text,
	 * they are marked as what was written is. Not marked, they passed for what
	 * the row's end was erased in, and reached the last column of any width
	 * after. */
	vt = mk(10, 4, 10);
	w(vt, "\033[44mabcd\033[4X\033[m\r\nX");
	CHECK(!vt->pri[0]->c[4].attr && vt->pri[0]->c[7].bg == 4 && vt->pri[0]->used == 8);
	CHECK(vt_resize(vt, 4, 4));
	CHECK(vt->pri[0]->wrap == 4 && vt->pri[1]->used == 4 && vt->pri[1]->wrap == 0);
	CHECK(vt->pri[1]->c[0].attr == ATTR_TEXT && vt->pri[1]->c[3].attr == ATTR_TEXT && vt->pri[1]->c[3].bg == 4);
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt_resize(vt, 10, 4));
	CHECK(vt->pri[0]->used == 8 && vt->pri[0]->c[7].bg == 4 && vt->pri[0]->c[8].bg == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 3, 4));
	CHECK(vt->pri[0]->wrap == 3 && vt->pri[1]->wrap == 3 && vt->pri[2]->used == 2 && vt->pri[2]->c[1].bg == 4);
	CHECK_STR(row(vt, 3), "X");
	vt_free(vt);
	/* Not those that are text only for the cursor behind them: with the
	 * cursor gone they are what the row was erased in again, and take no rows. */
	vt = mk(10, 3, 10);
	w(vt, "ab\033[44m\033[K\033[m\033[1;7H");
	CHECK(vt_resize(vt, 8, 3));
	CHECK(vt->c.x == 6 && vt->pri[0]->used == 8 && !vt->pri[0]->c[3].attr && vt->pri[0]->c[7].bg == 4);
	w(vt, "\033[2;1H");
	CHECK(vt_resize(vt, 4, 3));
	CHECK(vt->pri[0]->used == 4 && vt->pri[0]->wrap == 0 && vt->pri[0]->c[3].bg == 4 && !vt->pri[0]->c[3].attr);
	CHECK(vt->pri[1]->used == 0 && vt->c.y == 1);
	vt_free(vt);
	/* A row with such a colour is not empty, as it was not with the cells that
	 * held it: the text ends there, and the screen shows it rather than the
	 * rows above the cursor's. */
	vt = mk(6, 3, 10);
	w(vt, "abcd\033[2;4H\033[44m\033[K\033[m\033[1;5H");
	CHECK(vt_resize(vt, 3, 3));
	CHECK(vt->hist_count == 0 && vt->c.x == 1 && vt->c.y == 1);
	CHECK(vt->pri[2]->fill == 4 && vt->pri[2]->used == 3 && vt->pri[2]->c[2].bg == COL_DEFAULT_BG);
	CHECK(vt_resize(vt, 2, 3));
	CHECK(vt->hist_count == 1 && vt->c.x == 1 && vt->c.y == 0 && vt->c.wrapnext);
	CHECK_STR(row(vt, 0), "cd");
	CHECK(vt->pri[1]->wrap == 2 && vt->pri[2]->used == 2 && vt->pri[2]->c[1].bg == 4);
	vt_free(vt);
	/* nor where they fill the row to its end: they are of what the row was
	 * erased in themselves, and the row needs no colour of its own */
	vt = mk(10, 3, 10);
	w(vt, "ab\033[44m\033[K\033[m\033[1;7H");
	CHECK(vt_resize(vt, 6, 3));
	CHECK(vt->c.x == 5 && vt->c.wrapnext && vt->pri[0]->used == 6);
	CHECK(vt->pri[0]->fill == COL_DEFAULT_BG && !vt->pri[0]->c[5].attr && vt->pri[0]->c[5].bg == 4);
	w(vt, "\033[2;1H");
	CHECK(vt_resize(vt, 3, 3));
	CHECK(vt->pri[0]->used == 3 && vt->pri[0]->wrap == 0 && vt->pri[0]->c[2].bg == 4 && vt->pri[1]->used == 0);
	vt_free(vt);
	/* What a program writes is text by every way: a run of ASCII, one
	 * character, DEC graphics, a wide glyph and its other half, REP. */
	vt = mk(12, 2, 0);
	w(vt, "ab\xc3\xa4\033(0q\033(B\xe6\xbc\xa2 \033[3b");
	for (int x = 0; x < 10; x++)
		CHECK(vt->pri[0]->c[x].attr & ATTR_TEXT);
	CHECK(!vt->pri[0]->c[10].attr && vt->pri[0]->used == 10);
	/* not what an erase leaves, nor half a wide glyph mended */
	w(vt, "\033[1;1H\033[2X\033[1;6H\033[X");
	CHECK(!vt->pri[0]->c[0].attr && !vt->pri[0]->c[1].attr && (vt->pri[0]->c[2].attr & ATTR_TEXT));
	CHECK(vt->pri[0]->c[4].cp == ' ' && !vt->pri[0]->c[4].attr && !vt->pri[0]->c[5].attr);
	vt_free(vt);
	/* A blank that was written and shows nothing is as one that was not: the
	 * cell a wide glyph did not fit in is still left out of the line. */
	vt = mk(5, 3, 10);
	w(vt, "abcd \033[1;5H\xe6\xbc\xa2");
	CHECK(vt->pri[0]->c[4].attr == ATTR_TEXT && vt->pri[0]->wrap == 4 && vt_abs_wraps(vt, 0) == 4);
	vt_free(vt);
	/* Blanks with a colour or an attribute that do not reach the last column
	 * are text, a default blank after them or not. */
	vt = mk(10, 4, 10);
	w(vt, "ab\033[7m   \033[m\r\nX");
	CHECK(vt_resize(vt, 3, 4));
	CHECK(vt->pri[0]->wrap == 3 && (vt->pri[0]->c[2].attr & ATTR_REVERSE));
	CHECK(vt->pri[1]->used == 2 && (vt->pri[1]->c[1].attr & ATTR_REVERSE) && vt->pri[1]->wrap == 0);
	CHECK_STR(row(vt, 2), "X");
	vt_free(vt);
	/* only blanks like the last one are what the row was erased with: others
	 * before them, here underlined, are text and stay as they are */
	vt = mk(10, 3, 10);
	w(vt, "ab\033[41;4m  \033[24m\033[K\033[m\r\nX");
	CHECK(vt_resize(vt, 9, 3));
	CHECK((vt->pri[0]->c[2].attr & ATTR_UNDERLINE) && (vt->pri[0]->c[3].attr & ATTR_UNDERLINE));
	CHECK(!(vt->pri[0]->c[4].attr & ATTR_UNDERLINE) && vt->pri[0]->c[8].bg == 1);
	CHECK(vt_resize(vt, 3, 3));
	CHECK(vt->pri[1]->c[0].attr & ATTR_UNDERLINE);
	CHECK(vt->pri[1]->c[1].bg == 1 && !(vt->pri[1]->c[1].attr & ATTR_UNDERLINE) && vt->pri[1]->used == 3);
	vt_free(vt);
	/* blanks written with an attribute are text, up to the last column too: a
	 * status line in reverse takes the rows its width needs */
	vt = mk(10, 3, 10);
	w(vt, "\033[7mfile.c    \033[m\r\nX");
	CHECK(vt->pri[0]->used == 10);
	CHECK(vt_resize(vt, 8, 3));
	CHECK(vt->pri[0]->wrap == 8 && vt->pri[1]->used == 2 && (vt->pri[1]->c[1].attr & ATTR_REVERSE));
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt_resize(vt, 10, 3));
	CHECK(vt->pri[0]->used == 10 && vt->pri[0]->wrap == 0 && (vt->pri[0]->c[9].attr & ATTR_REVERSE));
	CHECK_STR(row(vt, 1), "X");
	vt_free(vt);
	/* with a background as well, which an erase does not leave with one */
	vt = mk(10, 3, 10);
	w(vt, "ab\033[41;4m        \033[m\r\nX");
	CHECK(vt_resize(vt, 9, 3));
	CHECK(vt->pri[0]->wrap == 9 && vt->pri[1]->used == 1 && (vt->pri[1]->c[0].attr & ATTR_UNDERLINE));
	vt_free(vt);
	/* and so are those with a foreground, as a label's padding */
	vt = mk(10, 3, 10);
	w(vt, "\033[37;41m  error   \033[m\r\nX");
	CHECK(vt_resize(vt, 9, 3));
	CHECK(vt->pri[0]->wrap == 9 && vt->pri[1]->used == 1 && vt->pri[1]->c[0].bg == 1);
	vt_free(vt);

	/* A wrap that is pending stays so in a window of another height: the next
	 * character goes to the next row and does not write over the last. The
	 * saved cursor's too, and on the alternate screen. */
	vt = mk(4, 3, 10);
	w(vt, "abcd");
	CHECK(vt_resize(vt, 4, 5));
	CHECK(vt->c.x == 3 && vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "abcd");
	CHECK_STR(row(vt, 1), "X");
	CHECK(vt_abs_wraps(vt, 0) == 4);
	vt_free(vt);
	vt = mk(4, 5, 10);
	w(vt, "abcd\0337\r\nx");
	CHECK(vt_resize(vt, 4, 3));
	w(vt, "\0338");
	CHECK(vt->c.x == 3 && vt->c.y == 0 && vt->c.wrapnext);
	w(vt, "\033[?47h\033[Hwxyz");
	CHECK(vt_resize(vt, 4, 2));
	w(vt, "Q");
	CHECK_STR(row(vt, 0), "wxyz");
	CHECK_STR(row(vt, 1), "Q");
	vt_free(vt);

	vt = mk(4, 3, 10);
	w(vt, "\033[?47h\033[Habcd\0337\r\nx");
	CHECK(vt_resize(vt, 4, 4));
	w(vt, "\0338");
	CHECK(vt->c.x == 3 && vt->c.y == 0 && vt->c.wrapnext);
	vt_free(vt);

	/* The saved cursor behind text that fills the new row waits there too,
	 * however far behind it was, and is not on the last character. */
	vt = mk(10, 3, 10);
	w(vt, "abc\033[2C\0337\r\nxy");
	CHECK(vt_resize(vt, 3, 3));
	w(vt, "\0338");
	CHECK(vt->c.x == 2 && vt->c.wrapnext);
	w(vt, "Z");
	CHECK_STR(row(vt, 0), "abc");
	vt_free(vt);

	/* The cells before the cursor are text in a row that holds nothing as
	 * well: clamped into a narrower row, the cursor was not back in its column
	 * in a wider one, and by way of a narrow window it ended elsewhere than
	 * without. */
	vt = mk(20, 3, 10);
	w(vt, "\033[1;18H");
	CHECK(vt_resize(vt, 12, 3));
	CHECK(vt->c.x == 5 && vt->c.y == 1 && vt->pri[0]->wrap == 12);
	CHECK(vt_resize(vt, 3, 3));
	CHECK(vt_resize(vt, 30, 3));
	CHECK(vt->c.x == 17 && vt->c.y == 0 && vt->hist_count == 0);
	vt_free(vt);

	/* A wrap pending on a row that goes on, where the cursor came back to
	 * after the wrap: the next character goes to the first cell of the next
	 * row, and there the cursor is in another width. */
	vt = mk(2, 3, 10);
	w(vt, "ab\0337c\0338");
	CHECK(vt->c.x == 1 && vt->c.y == 0 && vt->c.wrapnext && vt->pri[0]->wrap == 2);
	CHECK(vt_resize(vt, 3, 3));
	CHECK_STR(row(vt, 0), "abc");
	CHECK(vt->c.x == 2 && vt->c.y == 0 && !vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "abX");
	vt_free(vt);

	/* An empty row a line went on into, erased since, stays a row: the line
	 * goes on into it from where its text ends now, and nothing below it moves
	 * up. Back at the first width all is as it was. */
	vt = mk(5, 3, 10);
	w(vt, "abcdefg\033[2K\r\nX");
	CHECK(vt->pri[0]->wrap == 5 && vt->c.x == 1 && vt->c.y == 2);
	CHECK(vt_resize(vt, 6, 3));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt_abs_wraps(vt, 0) == 5 && vt->c.x == 1 && vt->c.y == 2 && vt->hist_count == 0);
	CHECK(vt_resize(vt, 3, 3));
	CHECK_STR(hist_line(vt, 1), "abc");
	CHECK_STR(row(vt, 0), "de");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt->pri[0]->wrap == 2 && vt->hist_count == 1 && vt->c.y == 2);
	CHECK(vt_resize(vt, 5, 3));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt->pri[0]->wrap == 5 && vt->hist_count == 0 && vt->c.x == 1 && vt->c.y == 2);
	vt_free(vt);
	/* the same for a row that holds only the blanks its line ended with */
	vt = mk(5, 3, 10);
	w(vt, "abcde  \r\nX");
	CHECK(vt->pri[0]->wrap == 5);
	CHECK(vt_resize(vt, 4, 3));
	CHECK(vt->hist_count == 1);
	CHECK_STR(hist_line(vt, 1), "abcd");
	CHECK_STR(row(vt, 0), "e");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt_resize(vt, 5, 3));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK_STR(row(vt, 1), "");
	CHECK_STR(row(vt, 2), "X");
	CHECK(vt->hist_count == 0);
	vt_free(vt);

	/* A last row whose next fell off the screen ends its line: what is
	 * written below it later is not joined to it by the next width. */
	vt = mk(10, 3, 10);
	w(vt, "0123456789abcde\033[1;4H");
	CHECK(vt_resize(vt, 4, 3));
	CHECK_STR(row(vt, 2), "89ab");
	CHECK(vt->pri[1]->wrap == 4 && vt->pri[2]->wrap == 0);
	w(vt, "\033[3;1H\r\nXY");
	CHECK(vt_resize(vt, 20, 3));
	CHECK_STR(row(vt, 0), "0123456789ab");
	CHECK_STR(row(vt, 1), "XY");
	vt_free(vt);
	/* in a shorter window too, on either screen */
	vt = mk(5, 3, 10);
	w(vt, "abcdefgh\033[H");
	CHECK(vt->pri[0]->wrap == 5);
	CHECK(vt_resize(vt, 5, 1));
	CHECK_STR(row(vt, 0), "abcde");
	CHECK(vt->pri[0]->wrap == 0);
	vt_free(vt);
	vt = mk(5, 3, 10);
	w(vt, "\033[?47h\033[Habcdefgh\033[H");
	CHECK(vt->alt[0]->wrap == 5);
	CHECK(vt_resize(vt, 5, 1));
	CHECK(vt->alt[0]->wrap == 0);
	vt_free(vt);
	/* not a row that is still followed by its next */
	vt = mk(5, 3, 10);
	w(vt, "abcdefgh\033[H");
	CHECK(vt_resize(vt, 5, 2));
	CHECK(vt->pri[0]->wrap == 5);
	vt_free(vt);

	/* A row of blanks that goes on in the next is text, and the end of the
	 * screen's text when nothing comes after it: narrower, it takes the rows
	 * it needs, and what is above the cursor goes into the history for them. */
	vt = mk(5, 4, 10);
	w(vt, "ab\r\n\r\n       \033[2;1H");
	CHECK(vt->pri[2]->wrap == 5 && vt->c.y == 1);
	CHECK(vt_resize(vt, 2, 4));
	CHECK(vt->hist_count == 1 && vt->c.x == 0 && vt->c.y == 0);
	CHECK_STR(hist_line(vt, 1), "ab");
	vt_free(vt);

	/* One column has no room for a wide glyph. A cursor on it is behind what
	 * came before, and the next character does not write over that. */
	vt = mk(10, 3, 10);
	w(vt, "a\xe6\xbc\xa2\033[1;2H");
	CHECK(vt_resize(vt, 1, 3));
	CHECK_STR(row(vt, 0), "a");
	CHECK(vt->c.x == 0 && vt->c.y == 0 && vt->c.wrapnext);
	w(vt, "X");
	CHECK_STR(row(vt, 0), "a");
	CHECK_STR(row(vt, 1), "X");
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "a\xe6\xbc\xa2\033[1;3H"); /* on its right half */
	CHECK(vt_resize(vt, 1, 3));
	CHECK(vt->c.x == 0 && vt->c.y == 0 && vt->c.wrapnext);
	vt_free(vt);
	vt = mk(10, 3, 10);
	w(vt, "\xe6\xbc\xa2\033[1;2H");
	CHECK(vt_resize(vt, 1, 3));
	CHECK(vt->c.x == 0 && vt->c.y == 0 && !vt->c.wrapnext);
	vt_free(vt);
}

static void test_index_and_charsets(void)
{
	struct vt *vt = mk(5, 3, 10);
	/* IND keeps the column, and scrolls at the bottom */
	w(vt, "a\033Db");
	CHECK(vt->pri[1]->c[1].cp == 'b' && vt->c.y == 1 && vt->c.x == 2);
	w(vt, "\033[3;2H\033D");
	CHECK(vt->c.y == 2 && vt->c.x == 1 && vt->hist_count == 1);
	CHECK_STR(row(vt, 0), " b");
	/* NEL goes to the start of the next row, and scrolls too */
	w(vt, "\033[1;4H\033E");
	CHECK(vt->c.y == 1 && vt->c.x == 0);
	w(vt, "\033[3;4H\033E");
	CHECK(vt->c.y == 2 && vt->c.x == 0 && vt->hist_count == 2);
	vt_free(vt);

	vt = mk(10, 3, 0);
	/* G1 is ASCII until it is set: SO alone changes nothing */
	w(vt, "\016q\017");
	CHECK(vt->pri[0]->c[0].cp == 'q');
	/* with G1 as line drawing SO and SI switch, and G0 stays */
	w(vt, "\033)0q\016q\017q");
	CHECK(vt->pri[0]->c[1].cp == 'q' && vt->pri[0]->c[2].cp == 0x2500 && vt->pri[0]->c[3].cp == 'q');
	/* G0 as line drawing does not touch G1 */
	w(vt, "\033)B\033(0q\016q\017q\033(B");
	CHECK(vt->pri[0]->c[4].cp == 0x2500 && vt->pri[0]->c[5].cp == 'q' && vt->pri[0]->c[6].cp == 0x2500);
	/* DECSC and DECRC take the charsets along, and the position */
	w(vt, "\033[2;1H\033)0\016\0337\017\033)Bq");
	CHECK(vt->pri[1]->c[0].cp == 'q' && vt->c.gl == 0);
	w(vt, "\0338q");
	CHECK(vt->pri[1]->c[0].cp == 0x2500 && vt->c.gl == 1 && vt->c.x == 1);
	vt_free(vt);
}

/* the display draws dirty rows, and a new cursor shape must reach the screen */
static void test_cursor_style_redraw(void)
{
	struct vt *vt = mk(10, 3, 0);
	w(vt, "\033[2;4H");
	vt->dirty[1] = false;
	w(vt, "\033[4 q");
	CHECK(vt->cursor_style == CURSOR_UNDERLINE && vt->dirty[1]);
	vt_free(vt);
}

static void test_origin(void)
{
	struct vt *vt = mk(10, 6, 0);
	w(vt, "\033[3;5r\033[?6h\033[1;1Ho\033[10;1Hp");
	CHECK(vt->pri[2]->c[0].cp == 'o');
	CHECK(vt->pri[4]->c[0].cp == 'p');
	w(vt, "\033[6n");
	CHECK_STR(replybuf, "\033[3;2R");
	vt_free(vt);
}

/* Where mwt follows xterm, from the 2026-11-17 review */
static void test_xterm_details(void)
{
	struct vt *vt = mk(5, 6, 0);
	/* parameters past the 24th are dropped, digits included: 0;x24 3;1 was red.
	 * xterm keeps 30, so there it is bold italic; dropping is DEC's rule */
	w(vt, "\033[0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;0;3;1mX");
	CHECK(vt->pri[0]->c[0].fg == COL_DEFAULT_FG);
	w(vt, "\033[1;3;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;5H");
	CHECK(vt->c.y == 0 && vt->c.x == 2);

	/* DECXCPR is relative to the scroll region in origin mode, as CPR */
	w(vt, "\033[3;5r\033[?6h\033[2;2H");
	replybuf[0] = 0;
	w(vt, "\033[6n\033[?6n");
	CHECK_STR(replybuf, "\033[2;2R\033[?2;2R");
	w(vt, "\033[?6l\033[r\033[2J\033[H");

	/* EL and ECH drop a pending wrap, as in xterm 407: x lands in the last column */
	w(vt, "abcde\033[Kx");
	CHECK_STR(row(vt, 0), "abcdx");
	CHECK_STR(row(vt, 1), "");
	w(vt, "\033[2;1Habcde\033[Xx");
	CHECK_STR(row(vt, 1), "abcdx");
	/* so do ED 0 to 2, checked against xterm 407; ED 3 erases only history */
	w(vt, "\033[2J\033[Habcde\033[1Jx");
	CHECK_STR(row(vt, 0), "    x");
	w(vt, "\033[2J\033[Habcde\033[3Jx");
	CHECK_STR(row(vt, 1), "x");
	w(vt, "\033[2J\033[H");
	/* ESC = and ESC > (smkx, is2) keep it, and so does DECSC/DECRC */
	w(vt, "\033[3;1Habcde\033=x");
	CHECK_STR(row(vt, 2), "abcde");
	CHECK_STR(row(vt, 3), "x");
	w(vt, "\033[5;1Habcde\0337\0338x");
	CHECK_STR(row(vt, 4), "abcde");
	CHECK_STR(row(vt, 5), "x");
	vt_free(vt);

	/* 47 never clears; 1047 clears the alternate screen when leaving it */
	vt = mk(10, 3, 0);
	w(vt, "\033[?47hALT\033[?47l\033[?1047h");
	CHECK_STR(row(vt, 0), "ALT");
	w(vt, "\033[?1047l\033[?47h");
	CHECK_STR(row(vt, 0), "");
	w(vt, "\033[?47l");
	CHECK_STR(row(vt, 0), "");
	vt_free(vt);
}

/* The ASCII fast path in vt_write writes runs; it must leave what put() would */
static void test_ascii_runs(void)
{
	struct vt *vt = mk(5, 3, 0);
	/* a run starting on a wide glyph's right half, one ending on a left half */
	w(vt, "\xe4\xb8\x80xy\033[1;2Hab");
	CHECK_STR(row(vt, 0), " aby");
	CHECK(!(vt->pri[0]->c[0].attr & (ATTR_WIDE | ATTR_WDUMMY)));
	w(vt, "\033[2;4H\xe4\xb8\x80\033[2;1Habcd");
	CHECK_STR(row(vt, 1), "abcd");
	CHECK(!(vt->pri[1]->c[4].attr & (ATTR_WIDE | ATTR_WDUMMY)));
	/* a run to the end of the row defers the wrap, the next byte takes it */
	w(vt, "\033[3;1Habcde");
	CHECK(vt->c.x == 4 && vt->c.wrapnext);
	w(vt, "f"); /* the bottom row: the screen scrolls */
	CHECK_STR(row(vt, 1), "abcde");
	CHECK_STR(row(vt, 2), "f");
	CHECK(vt->lastcp == 'f');
	/* no autowrap: the run stops at the edge, the rest lands in the last column */
	w(vt, "\033[?7l\033[1;1Hvwxyz123");
	CHECK_STR(row(vt, 0), "vwxy3");
	w(vt, "\033[?7h");
	/* insert mode goes through put() */
	w(vt, "\033[2J\033[1;1Habc\033[1;1H\033[4hXY\033[4l");
	CHECK_STR(row(vt, 0), "XYabc");
	w(vt, "\033(0q\033(B");
	CHECK(vt->pri[0]->c[2].cp == 0x2500 && (vt->pri[0]->c[2].attr & ATTR_DECGFX));
	/* DEC graphics in a run: from 0x5f mapped and marked, below as they are,
	 * with the template's attributes; REP repeats the byte, mapped again */
	w(vt, "\033[2J\033[1;1H\033[1m\033(0Aqx_\033(B\033[0m");
	CHECK(vt->pri[0]->c[0].cp == 'A' && !(vt->pri[0]->c[0].attr & ATTR_DECGFX) &&
	      (vt->pri[0]->c[0].attr & ATTR_BOLD));
	CHECK(vt->pri[0]->c[1].cp == 0x2500 && vt->pri[0]->c[2].cp == 0x2502 && vt->pri[0]->c[3].cp == 0xa0);
	CHECK((vt->pri[0]->c[1].attr & (ATTR_DECGFX | ATTR_BOLD)) == (ATTR_DECGFX | ATTR_BOLD));
	w(vt, "\033[1;1H\033(0q\033[b\033(B");
	CHECK(vt->pri[0]->c[1].cp == 0x2500 && (vt->pri[0]->c[1].attr & ATTR_DECGFX));
	/* attributes and colour from the template, on every cell of the run */
	w(vt, "\033[2;1H\033[1;31mabc\033[0m");
	CHECK(vt->pri[1]->c[0].fg == 1 && vt->pri[1]->c[2].fg == 1 && (vt->pri[1]->c[2].attr & ATTR_BOLD));
	vt_free(vt);
}

/* CSI parameters skip feed() in vt_write; split anywhere they must add up the same */
static void test_csi_params(void)
{
	struct vt *vt = mk(20, 10, 0);
	const char *seq = "\033[5;12H\033[38:2:1:2:3mx";
	for (size_t cut = 0; cut <= strlen(seq); cut++) {
		w(vt, "\033[0m\033[H");
		vt_write(vt, seq, cut);
		vt_write(vt, seq + cut, strlen(seq) - cut);
		CHECK(vt->c.y == 4 && vt->c.x == 12);
		CHECK(vt->pri[4]->c[11].cp == 'x' && vt->pri[4]->c[11].fg == (COL_RGB | 0x010203));
	}
	/* a private marker after the parameters makes the sequence ignored */
	w(vt, "\033[?25l\033[H\033[1;?25hA");
	CHECK((vt->mode & MODE_HIDECURSOR) && vt->pri[0]->c[0].cp == 'A');
	w(vt, "\033[?25h");
	/* parameters clamp at 65535 however they come */
	w(vt, "\033[H\033[99999999999");
	w(vt, "99C");
	CHECK(vt->c.x == 19);
	vt_free(vt);
}

/* Sequences that are not quite the ones they look like. */
static void test_malformed_sequences(void)
{
	struct vt *vt = mk(20, 3, 0);
	/* a second intermediate makes another sequence: ESC ( % 0 is DEC Turkish,
	 * not the graphics, nor is ESC ) % 0 for G1 */
	w(vt, "\033(%0q\033)%0");
	CHECK(vt->c.charset[0] == 0 && vt->c.charset[1] == 0);
	CHECK_STR(row(vt, 0), "q");
	/* so with CSI, and with more of them than are kept */
	replybuf[0] = 0;
	w(vt, "\033[?1h\033[4 !q\033[4 !\"#q\033[!\"p\033[>!c\033[> q\033[4$!p\033[?7$!p");
	CHECK(vt->cursor_style == CURSOR_BLOCK);
	CHECK(vt->mode & MODE_APPCURSOR); /* no DECSTR */
	CHECK_STR(replybuf, "");
	/* with one, or none, they are what they were */
	w(vt, "\033[4 q\033[>c");
	CHECK(vt->cursor_style == CURSOR_UNDERLINE);
	CHECK_STR(replybuf, "\033[>1;10;0c");

	/* a character that is not ASCII ends a sequence wherever it comes, also
	 * after an intermediate and in a sequence that is ignored: the rest is
	 * text (CAVEATS.md) */
	w(vt, "\033[2J\033[H\033(\xc3\xa4" "0q");
	CHECK(vt->c.charset[0] == 0);
	CHECK_STR(row(vt, 0), "0q");
	w(vt, "\033[2K\r\033[6 \xc3\xa4q");
	CHECK(vt->cursor_style == CURSOR_UNDERLINE);
	CHECK_STR(row(vt, 0), "q");
	w(vt, "\033[2K\r\033[1?\xc3\xa4hx");
	CHECK_STR(row(vt, 0), "hx");
	/* DEL does not */
	w(vt, "\033[2K\r\033[6 \x7fq\033(\x7f" "0q\033(B");
	CHECK(vt->cursor_style == CURSOR_BAR);
	CHECK(vt->pri[0]->c[0].cp == 0x2500 && vt->pri[0]->c[1].cp == ' ');

	/* sub-parameters are SGR's alone: with a ':' anything else is ignored, also
	 * when it comes after the last parameter that is kept */
	replybuf[0] = 0;
	w(vt, "\033[H\033[2:5H\033[2:1B\033[?25:1l\033[5:1n\033[0:1c\033[?7:1$p");
	CHECK(vt->c.x == 0 && vt->c.y == 0 && !(vt->mode & MODE_HIDECURSOR));
	CHECK_STR(replybuf, "");
	w(vt, "\033[2;2;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1:1H");
	CHECK(vt->c.x == 0 && vt->c.y == 0);
	/* the next sequence is not held to it, and SGR keeps them */
	w(vt, "\033[2;3H\033[4:3mU");
	CHECK(vt->c.y == 1 && vt->c.x == 3);
	CHECK(vt->pri[1]->c[2].cp == 'U' && (vt->pri[1]->c[2].attr & ATTR_UNDERLINE));
	vt_free(vt);
}

/* widths from the table, filled a block of 256 at a time: as wcwidth() says, for
 * every code point of the BMP that prints (a combining mark goes nowhere) */
static void test_width_table(void)
{
	struct vt *vt = mk(4, 2, 0);
	int wrong = 0;
	for (uint32_t cp = 0xa0; cp <= 0xffff; cp++) {
		if (cp >= 0xd800 && cp < 0xe000)
			continue;
		char b[8] = "\033[H";
		size_t n = 3;
		if (cp < 0x800) {
			b[n++] = (char)(0xc0 | cp >> 6);
		} else {
			b[n++] = (char)(0xe0 | cp >> 12);
			b[n++] = (char)(0x80 | (cp >> 6 & 0x3f));
		}
		b[n++] = (char)(0x80 | (cp & 0x3f));
		vt_write(vt, b, n);
		int want = cp < 0x300 ? 1 : wcwidth((wchar_t)cp) < 0 ? 1 : wcwidth((wchar_t)cp);
		wrong += vt->c.x != want;
	}
	CHECK(wrong == 0);
	vt_free(vt);
}

/* Scrolled back, dirty marks the rows shown: screen row y is in row y + view */
static void test_dirty_scrolled_back(void)
{
	struct vt *vt = mk(10, 4, 20);
	for (int i = 0; i < 10; i++) {
		char b[16];
		snprintf(b, sizeof b, "a%d\r\n", i);
		w(vt, b);
	}
	vt_scroll_view(vt, 2);
	memset(vt->dirty, 0, 4);
	w(vt, "\0337\033[1;1HX\0338"); /* screen row 0, shown in row 2 */
	CHECK_STR(row(vt, 2), "X7");
	CHECK(vt->dirty[2] && !vt->dirty[0] && !vt->dirty[1]);
	/* a line scrolled off: the history shown stays, the rows below move */
	memset(vt->dirty, 0, 4);
	w(vt, "b\r\n");
	CHECK(vt->view == 3);
	CHECK(!vt->dirty[0] && !vt->dirty[1] && !vt->dirty[2] && vt->dirty[3]);
	vt_free(vt);

	/* at the top of a full history the text shown moves: all of it */
	vt = mk(10, 4, 3);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n");
	vt_scroll_view(vt, 100);
	CHECK(vt->view == 3);
	memset(vt->dirty, 0, 4);
	w(vt, "\r\n");
	CHECK(vt->dirty[0] && vt->dirty[3]);
	vt_free(vt);

	/* a region above a footer (DECSTBM): the history grows, the view moves,
	 * and the footer below the region is shown a row lower */
	vt = mk(10, 6, 20);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n");
	w(vt, "\033[4;1HF3\033[5;1HF4\033[6;1HF5\033[1;3r\033[3;1H");
	vt_scroll_view(vt, 1);
	CHECK_STR(row(vt, 4), "F3");
	CHECK_STR(row(vt, 5), "F4");
	memset(vt->dirty, 0, 6);
	w(vt, "\n");
	CHECK(vt->view == 2);
	CHECK_STR(row(vt, 5), "F3");
	CHECK(vt->dirty[5]);
	/* with the view at the bottom nothing below the region moves */
	vt_scroll_view(vt, -100);
	memset(vt->dirty, 0, 6);
	w(vt, "\n");
	CHECK(!vt->dirty[3] && !vt->dirty[4] && !vt->dirty[5]);
	vt_free(vt);
}

/* Section 8 of the review of 2026-11-18: mutants of vt.c these caught none of. */

/* ESC D, E and M drop a pending wrap, as they move the cursor; ESC H keeps it,
 * and so does ESC Z, which is nothing here. */
static void test_wrap_flag_esc(void)
{
	struct vt *vt = mk(10, 4, 0);
	w(vt, "0123456789\033Dy");
	CHECK(vt->pri[1]->c[9].cp == 'y' && vt->pri[2]->c[0].cp == ' ');
	w(vt, "\033c0123456789\033Ey");
	CHECK(vt->pri[1]->c[0].cp == 'y' && vt->pri[2]->c[0].cp == ' ');
	w(vt, "\033c\033[2;1H0123456789\033My");
	CHECK(vt->pri[0]->c[9].cp == 'y' && vt->pri[1]->c[9].cp == '9');
	w(vt, "\033c0123456789\033Hy");
	CHECK(vt->pri[0]->c[9].cp == '9' && vt->pri[1]->c[0].cp == 'y');
	w(vt, "\033c0123456789\033Zy");
	CHECK(vt->pri[0]->c[9].cp == '9' && vt->pri[1]->c[0].cp == 'y');
	/* a tab forward leaves the wrap pending, as xterm 407: the cursor is in
	 * the last column and stays there; a tab back takes it, moving */
	w(vt, "\033c0123456789\ty");
	CHECK(vt->pri[0]->c[9].cp == '9' && vt->pri[1]->c[0].cp == 'y');
	w(vt, "\033c0123456789\033[2Iy");
	CHECK(vt->pri[0]->c[9].cp == '9' && vt->pri[1]->c[0].cp == 'y');
	w(vt, "\033c0123456789\033[Zy");
	CHECK(vt->pri[0]->c[8].cp == 'y' && vt->pri[0]->c[9].cp == '9' && vt->pri[1]->c[0].cp == ' ');
	CHECK(vt->c.x == 9 && vt->c.y == 0 && !vt->c.wrapnext);
	/* one that reaches the last column leaves none pending */
	w(vt, "\033c\033[1;9H\ty");
	CHECK(vt->pri[0]->c[9].cp == 'y' && vt->pri[1]->c[0].cp == ' ' && vt->c.wrapnext);
	vt_free(vt);
}

/* That a line goes on in the next row is the row's, not a cell's. Cells put in
 * before its end (ICH, insert mode, REP in it) push that end off the row and
 * leave the line going on, in the history too; it ends when its last cell is
 * written or erased, and with DCH before it, which empties that cell. After a
 * wider window the wrap is left of the end, and moves with what is inserted. */
static void test_wrap_row(void)
{
	struct vt *vt = mk(10, 4, 5);
	w(vt, "abcdefghijKLM\033[1;3H\033[@");
	CHECK_STR(row(vt, 0), "ab cdefghi");
	CHECK(vt_abs_wraps(vt, 0) == 10 && vt_abs_wraps(vt, 1) == 0);
	w(vt, "\033[4hX\033[4l");
	CHECK_STR(row(vt, 0), "abX cdefgh");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	w(vt, "\033[4hq\033[3b\033[4l");
	CHECK_STR(row(vt, 0), "abXqqqq cd");
	CHECK(vt_abs_wraps(vt, 0) == 10 && vt->pri[0]->used == 10);
	w(vt, "\033[4;1H\r\n");
	int len;
	const struct cell *l = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK(l && len == 10 && l[9].cp == 'd' && vt_abs_wraps(vt, vt_abs_first(vt)) == 10);
	vt_free(vt);

	/* half a wide glyph pushed off the end: what is left of it goes, the line
	 * goes on */
	vt = mk(10, 4, 0);
	w(vt, "abcdefgh\xe6\xbc\xa2K\033[1;1H\033[@");
	CHECK_STR(row(vt, 0), " abcdefgh");
	CHECK(vt->pri[0]->c[9].cp == ' ' && !vt->pri[0]->c[9].attr && vt_abs_wraps(vt, 0) == 10);
	vt_free(vt);

	/* The cell a wide glyph did not fit in is not the line's, which goes on
	 * after the nine before it, in the history too; put in before it, it moves
	 * along as the wrap does. With anything but a blank there, as a colored
	 * one, the cell is the line's as before. */
	vt = mk(10, 2, 5);
	w(vt, "abcdefghi\xe6\xbc\xa2");
	CHECK(vt->pri[0]->wrap == 9 && vt->pri[0]->used == 9 && vt_abs_wraps(vt, 0) == 9);
	w(vt, "\r\n");
	l = vt_abs_line(vt, vt_abs_first(vt), &len);
	CHECK(l && len == 9 && l[8].cp == 'i' && vt_abs_wraps(vt, vt_abs_first(vt)) == 9);
	vt_free(vt);
	vt = mk(10, 2, 5);
	w(vt, "abcdefghi\xe6\xbc\xa2\033[1;1H\033[@");
	CHECK_STR(row(vt, 0), " abcdefghi");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	vt_free(vt);
	vt = mk(10, 2, 5);
	w(vt, "\033[41m\033[K\033[mabcdefghi\xe6\xbc\xa2");
	CHECK(vt->pri[0]->wrap == 10 && vt_abs_wraps(vt, 0) == 10);
	vt_free(vt);
	/* The last cell before the wrap is the ninth then, and writing it ends the
	 * line as writing the last cell of a full row does (unwrap); before, with
	 * the cell left over counted, it took writing that one. Writing further
	 * left leaves the line going on. */
	vt = mk(3, 3, 5);
	w(vt, "ab\xe4\xb8\xad\033[1;2Hx");
	CHECK_STR(row(vt, 0), "ax");
	CHECK(vt->pri[0]->wrap == 0 && vt_abs_wraps(vt, 0) == 0);
	vt_free(vt);
	vt = mk(3, 3, 5);
	w(vt, "ab\xe4\xb8\xad\033[1;1Hx");
	CHECK_STR(row(vt, 0), "xb");
	CHECK(vt->pri[0]->wrap == 2 && vt_abs_wraps(vt, 0) == 2);
	vt_free(vt);

	static const struct { const char *seq; int wraps; } cases[] = {
		{ "\033[1;3HZ", 10 },                  /* written before the end */
		{ "\033[1;3H\033[2X", 10 },            /* erased there */
		{ "\033[1;5H\033[1K", 10 },
		{ "\033[1;3H\033[2@", 10 },
		{ "\033[1;9H\033[4hX\033[4l", 10 },
		{ "\033[1;3H\033[8@", 0 },             /* ICH to the end erases to it */
		{ "\033[1;10H\033[@", 0 },
		{ "\033[1;10H\033[4hX\033[4l", 0 },    /* the last cell written */
		{ "\033[1;10HX", 0 },
		{ "\033[1;10HXY", 10 },                /* and running into the end again */
		{ "\033[1;10H\033[X", 0 },             /* erased */
		{ "\033[1;5H\033[K", 0 },
		{ "\033[1;10H\033[1K", 0 },
		{ "\033[1;3H\033[P", 0 },              /* DCH */
		{ "\033[1;1H\033[M", 0 },              /* the row gone: KLM is the first */
		{ "\033[1;1H\033[L", 0 },              /* a new row above it */
		{ "\033[2J", 0 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
		vt = mk(10, 4, 0);
		w(vt, "abcdefghijKLM");
		w(vt, cases[i].seq);
		checks++;
		if (vt_abs_wraps(vt, 0) != cases[i].wraps) {
			failures++;
			fprintf(stderr, "%s:%d: FAIL: case %zu: wraps %d, expected %d\n", __FILE__,
				__LINE__, i, vt_abs_wraps(vt, 0), cases[i].wraps);
		}
		vt_free(vt);
	}

	/* A wider window on the alternate screen, which is not laid out anew: the
	 * line still ends after 10 cells, 12 with two put in before them; blanks
	 * put in after them change nothing, text there ends it, and a window
	 * narrower than the line cuts it */
	vt = mk(10, 4, 0);
	w(vt, "\033[?47habcdefghijKLM");
	CHECK(vt_resize(vt, 20, 4) && vt_abs_wraps(vt, 0) == 10);
	w(vt, "\033[1;13H\033[P");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	w(vt, "\033[1;3H\033[P");
	CHECK(vt_abs_wraps(vt, 0) == 0);
	w(vt, "\033[2J\033[H");
	vt_resize(vt, 10, 4);
	w(vt, "abcdefghijKLM");
	CHECK(vt_resize(vt, 20, 4) && vt_abs_wraps(vt, 0) == 10);
	w(vt, "\033[1;3H\033[2@");
	CHECK_STR(row(vt, 0), "ab  cdefghij");
	CHECK(vt_abs_wraps(vt, 0) == 12);
	w(vt, "\033[1;15H\033[2@");
	CHECK(vt_abs_wraps(vt, 0) == 12);
	w(vt, "\033[1;15H\033[4hX\033[4l");
	CHECK(vt_abs_wraps(vt, 0) == 0);
	w(vt, "\033[1;15H\033[X");
	CHECK(vt_abs_wraps(vt, 0) == 12);
	w(vt, "\033[1;3H\033[19@");
	CHECK(vt_abs_wraps(vt, 0) == 0);
	w(vt, "\033[2J\033[H0123456789abcdefghijK\033[1;1H\033[@");
	CHECK(vt_abs_wraps(vt, 0) == 20);
	CHECK(vt_resize(vt, 19, 4) && vt_abs_wraps(vt, 0) == 0);
	vt_free(vt);

	/* the first cell after the wrap is not the line's: written, the line ends
	 * in its row, and erased again it goes on as before */
	vt = mk(10, 4, 0);
	w(vt, "\033[?47habcdefghijKLM");
	CHECK(vt_resize(vt, 20, 4));
	w(vt, "\033[1;11HZ");
	CHECK(vt->alt[0]->wrap == 10 && vt_abs_wraps(vt, 0) == 0);
	w(vt, "\033[1;11H\033[X");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	vt_free(vt);

	/* a row has one wrap: wrapped, made wider and wrapped again at the new
	 * end, a narrower window cuts that off, and the first is not back */
	vt = mk(4, 3, 0);
	w(vt, "\033[?47habcdEF");
	CHECK(vt_resize(vt, 8, 3) && vt_abs_wraps(vt, 0) == 4);
	w(vt, "\033[1;7Hxyz");
	CHECK(vt_abs_wraps(vt, 0) == 8);
	CHECK(vt_resize(vt, 6, 3));
	CHECK_STR(row(vt, 0), "abcd");
	CHECK(vt_abs_wraps(vt, 0) == 0);
	vt_free(vt);

	/* blanks erased past what was written leave the count, and rows a taller
	 * window adds have none written */
	vt = mk(10, 2, 0);
	w(vt, "ab\033[1;6H\033[K");
	CHECK(vt->pri[0]->used == 2);
	CHECK(vt_resize(vt, 10, 4) && vt->pri[3]->used == 0 && vt->pri[3]->wrap == 0);
	vt_free(vt);

	/* a wide glyph before the wrap with its left half overwritten: the right
	 * half, the last cell of the line, goes, and the line ends there */
	vt = mk(10, 4, 0);
	w(vt, "abcdefgh\xe6\xbc\xa2KLM");
	CHECK(vt_abs_wraps(vt, 0) == 10);
	w(vt, "\033[1;9Hz");
	CHECK_STR(row(vt, 0), "abcdefghz");
	CHECK(vt_abs_wraps(vt, 0) == 0);
	vt_free(vt);
}

/* A resize in the alternate screen keeps each screen's cursor line, and the
 * primary's lines above go to the history; a view scrolled back comes down. */
static void test_resize_alt_and_view(void)
{
	struct vt *vt = mk(10, 5, 10);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\033[?1049ha\r\nb\r\nc\r\nd\r\ne");
	CHECK(vt->c.y == 4);
	vt_resize(vt, 6, 3);
	CHECK_STR(row(vt, 0), "c");
	CHECK_STR(row(vt, 2), "e");
	CHECK(vt->c.y == 2 && vt->hist_count == 2);
	w(vt, "\033[?1049l");
	CHECK_STR(row(vt, 0), "3");
	CHECK_STR(row(vt, 2), "5");
	CHECK(vt->c.y == 2 && vt->c.x == 1);
	/* scrolled back: the view is the live screen again after a resize */
	vt_scroll_view(vt, 2);
	CHECK(vt->view == 2);
	vt_resize(vt, 8, 3);
	CHECK(vt->view == 0);
	CHECK_STR(row(vt, 2), "5");
	vt_free(vt);
}

/* SGR 22 ends dim too, 4:0 ends the underline and 4:3 starts one, blinking (5
 * and 25) changes nothing, and sub-parameters of what mwt does not know are
 * skipped, not read as the next parameters. */
static void test_sgr_details(void)
{
	struct vt *vt = mk(20, 2, 0);
	w(vt, "\033[1;2ma\033[22mb\033[4mc\033[4:0md\033[4:3me\033[m"
	      "\033[5mf\033[25mg\033[m\033[1:2:3;31mh\033[m\033[73:2:3mi");
	struct cell *l = vt->pri[0]->c;
	CHECK((l[0].attr & ATTR_DIM) && !(l[1].attr & (ATTR_BOLD | ATTR_DIM)));
	CHECK((l[2].attr & ATTR_UNDERLINE) && !(l[3].attr & ATTR_UNDERLINE));
	CHECK(l[4].attr & ATTR_UNDERLINE);
	CHECK(l[5].cp == 'f' && l[5].attr == ATTR_TEXT && l[6].cp == 'g' && l[6].attr == ATTR_TEXT);
	CHECK((l[7].attr & ATTR_BOLD) && !(l[7].attr & (ATTR_DIM | ATTR_ITALIC)) && l[7].fg == 1);
	CHECK(!(l[8].attr & (ATTR_DIM | ATTR_ITALIC)));
	vt_free(vt);
}

/* CNL and CPL stop at the scroll region from inside it; mode 1048 saves and
 * restores the cursor; DECRQM tells origin mode (6). */
static void test_cnl_cpl_1048_decom(void)
{
	struct vt *vt = mk(10, 8, 0);
	w(vt, "\033[3;6r\033[4;5H\033[9E");
	CHECK(vt->c.y == 5 && vt->c.x == 0);
	w(vt, "\033[4;5H\033[9F");
	CHECK(vt->c.y == 2 && vt->c.x == 0);
	/* outside the region: the screen's edge */
	w(vt, "\033[8;5H\033[2F");
	CHECK(vt->c.y == 5);
	w(vt, "\033[1;5H\033[1F");
	CHECK(vt->c.y == 0);
	w(vt, "\033[r\033[2;3H\033[?1048h\033[6;7H\033[?1048l");
	CHECK(vt->c.y == 1 && vt->c.x == 2);
	replybuf[0] = 0;
	w(vt, "\033[?6$p\033[?6h\033[?6$p\033[?6l");
	CHECK_STR(replybuf, "\033[?6;2$y\033[?6;1$y");
	vt_free(vt);
}

/* Lines out of range: none, and no length, below the oldest in the history and
 * past the last of the screen. */
static void test_abs_line_range(void)
{
	struct vt *vt = mk(10, 3, 2);
	w(vt, "1\r\n2\r\n3\r\n4\r\n5\r\n6"); /* 3 lines gone, 2 kept */
	int len = 99;
	int64_t first = vt_abs_first(vt), last = (int64_t)vt->scrolled + vt->rows - 1;
	CHECK(vt->hist_count == 2);
	CHECK(vt_abs_line(vt, first, &len) && len > 0);
	CHECK(!vt_abs_line(vt, first - 1, &len) && len == 0);
	len = 99;
	CHECK(vt_abs_line(vt, last, &len) && len == 10);
	CHECK(!vt_abs_line(vt, last + 1, &len) && len == 0);
	len = 99;
	CHECK(!vt_abs_line(vt, INT64_MIN / 2, &len) && len == 0);
	vt_free(vt);
}

static void test_alloc_failures(void)
{
	/* vt_new: any one allocation failing gives NULL and leaks nothing */
	for (long n = 0;; n++) {
		injected = false;
		countdown = n;
		struct vt *vt = vt_new(20, 5, 10, NULL);
		countdown = -1;
		if (!injected) {
			CHECK(vt != NULL);
			vt_free(vt);
			break;
		}
		CHECK(vt == NULL);
	}
	/* vt_resize: a failure leaves the terminal as it was, and it goes on */
	int fails = 0;
	for (long n = 0;; n++) {
		struct vt *vt = mk(10, 4, 10);
		w(vt, "1\r\n2\r\n3\r\nabc");
		injected = false;
		countdown = n;
		bool ok = vt_resize(vt, 30, 2);
		countdown = -1;
		if (ok) { /* no failure, or only a history line was lost */
			CHECK(vt->cols == 30 && vt->rows == 2 && vt->c.y == 1);
			CHECK_STR(row(vt, 1), "abc");
		} else {
			fails++;
			CHECK(vt->cols == 10 && vt->rows == 4 && vt->hist_count == 0);
			CHECK(vt->c.x == 3 && vt->c.y == 3);
			CHECK_STR(row(vt, 0), "1");
			CHECK_STR(row(vt, 3), "abc");
			w(vt, "\r\nmore");
			CHECK_STR(row(vt, 3), "more");
			CHECK(vt->hist_count == 1);
		}
		vt_free(vt);
		if (!injected)
			break;
	}
	CHECK(fails > 0);
	/* the same behind the alternate screen: the main screen's row and its
	 * saved cursor are as they were */
	fails = 0;
	for (long n = 0;; n++) {
		struct vt *vt = mk(10, 6, 10);
		w(vt, "1\r\n2\r\n3\0337\r\n4\r\n5\r\n6\033[?47h\033[2;3H");
		injected = false;
		countdown = n;
		bool ok = vt_resize(vt, 10, 3);
		countdown = -1;
		if (ok) {
			CHECK(vt->rows == 3 && vt->pri_y == 2 && vt->saved_pri.y == 0);
		} else {
			fails++;
			CHECK(vt->rows == 6 && vt->pri_y == 5 && vt->saved_pri.y == 2);
			CHECK(vt->c.y == 1 && vt->c.x == 2 && vt->hist_count == 0);
		}
		vt_free(vt);
		if (!injected)
			break;
	}
	CHECK(fails > 0);
	/* the same for a taller one: the history keeps its lines when it fails */
	fails = 0;
	for (long n = 0;; n++) {
		struct vt *vt = mk(10, 2, 10);
		w(vt, "1\r\n2\r\n3\r\nabc");
		injected = false;
		countdown = n;
		bool ok = vt_resize(vt, 10, 4);
		countdown = -1;
		if (ok) {
			CHECK(vt->rows == 4 && vt->c.y == 3 && vt->hist_count == 0);
			CHECK_STR(row(vt, 0), "1");
			CHECK_STR(row(vt, 3), "abc");
		} else {
			fails++;
			CHECK(vt->rows == 2 && vt->hist_count == 2 && vt->scrolled == 2);
			CHECK(vt->c.x == 3 && vt->c.y == 1);
			CHECK_STR(hist_line(vt, 1), "2");
			CHECK_STR(row(vt, 0), "3");
			CHECK_STR(row(vt, 1), "abc");
		}
		vt_free(vt);
		if (!injected)
			break;
	}
	CHECK(fails > 0);
	/* and for another width, where the history is made anew as well: every
	 * row of it and of the screen, and the terminal is as it was */
	fails = 0;
	for (long n = 0;; n++) {
		struct vt *vt = mk(10, 2, 10);
		w(vt, "0123456789abcdefghijKLM\r\nxy");
		injected = false;
		countdown = n;
		bool ok = vt_resize(vt, 5, 3);
		countdown = -1;
		if (ok) {
			CHECK(!injected && vt->cols == 5 && vt->rows == 3 && vt->hist_count == 3);
			CHECK(vt->c.x == 2 && vt->c.y == 2 && vt->scrolled == 3);
			CHECK_STR(hist_line(vt, 3), "01234");
			CHECK_STR(row(vt, 0), "fghij");
			CHECK_STR(row(vt, 2), "xy");
		} else {
			fails++;
			CHECK(vt->cols == 10 && vt->rows == 2 && vt->hist_count == 2 && vt->scrolled == 2);
			CHECK(vt->c.x == 2 && vt->c.y == 1);
			CHECK_STR(hist_line(vt, 2), "0123456789");
			CHECK(vt_abs_wraps(vt, 0) == 10 && vt_abs_wraps(vt, 1) == 10);
			CHECK_STR(row(vt, 0), "KLM");
			CHECK_STR(row(vt, 1), "xy");
			w(vt, "\r\nmore");
			CHECK_STR(row(vt, 1), "more");
			CHECK(vt->hist_count == 3);
		}
		vt_free(vt);
		if (!injected)
			break;
	}
	CHECK(fails > 5);
	/* a history line that cannot be kept goes in empty: it scrolled off all the
	 * same, so the numbers of the lines after it stay */
	{
		struct vt *vt = mk(10, 3, 10);
		w(vt, "a\r\nb\r\nc");
		injected = false;
		countdown = 0;
		w(vt, "\r\nd");
		countdown = -1;
		CHECK(injected);
		CHECK(vt->scrolled == 1 && vt->hist_count == 1);
		int len;
		vt_abs_line(vt, 0, &len);
		CHECK(len == 0);
		const struct cell *l = vt_abs_line(vt, 1, &len);
		CHECK(l && len == 10 && l[0].cp == 'b');
		w(vt, "\r\ne");
		l = vt_abs_line(vt, 1, &len);
		CHECK(l && len == 1 && l[0].cp == 'b');
		vt_free(vt);
	}
	/* and one that wrapped does not go on: it has no cells before a wrap */
	{
		struct vt *vt = mk(10, 3, 10);
		w(vt, "0123456789ab\r\nc");
		injected = false;
		countdown = 0;
		w(vt, "\r\nd");
		countdown = -1;
		CHECK(injected && vt->hist_count == 1);
		int len;
		vt_abs_line(vt, 0, &len);
		CHECK(len == 0 && vt_abs_wraps(vt, 0) == 0);
		vt_free(vt);
	}
	/* sizes are clamped, the largest grid still works */
	struct vt *vt = mk(INT_MAX, INT_MAX, 0);
	CHECK(vt && vt->cols == VT_MAX_COLS && vt->rows == VT_MAX_ROWS);
	w(vt, "\033[2048;4096Hz");
	CHECK(vt->pri[VT_MAX_ROWS - 1]->c[VT_MAX_COLS - 1].cp == 'z');
	CHECK(vt_resize(vt, -5, INT_MIN) && vt->cols == 1 && vt->rows == 1);
	vt_free(vt);
}

int main(void)
{
	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "");
	test_wrap();
	test_cursor_erase();
	test_scroll_history();
	test_row_extent();
	test_scroll_region_ops();
	test_sgr();
	test_wide();
	test_wide_insert();
	test_altscreen();
	test_replies();
	test_color_queries();
	test_keys();
	test_checksum();
	test_checksum_large();
	test_osc_controls();
	test_osc_long();
	test_charset_and_misc();
	test_unsupported_vt100();
	test_resize();
	test_resize_back();
	test_reflow();
	test_plain_output();
	test_reflow_ends();
	test_origin();
	test_xterm_details();
	test_ascii_runs();
	test_csi_params();
	test_malformed_sequences();
	test_width_table();
	test_dirty_scrolled_back();
	test_cursor_style_redraw();
	test_index_and_charsets();
	test_wrap_flag_esc();
	test_wrap_row();
	test_resize_alt_and_view();
	test_sgr_details();
	test_cnl_cpl_1048_decom();
	test_abs_line_range();
	test_alloc_failures();
	test_history_slots();
	test_rep();
	test_xterm_resets();
	test_fuzz();
	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
