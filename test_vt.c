/* test_vt.c - headless tests for the VT core: feed sequences, check the grid */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;

/* Fault injection, linked with --wrap: with countdown = n the n-th allocation
 * from vt.c (and this file) fails, all others succeed. */
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
static char replybuf[256];
static char titlebuf[256];

static void on_write(void *ud, const char *b, size_t n)
{
	(void)ud;
	strncat(replybuf, b, n);
}

static void on_title(void *ud, const char *t)
{
	(void)ud;
	snprintf(titlebuf, sizeof titlebuf, "%s", t);
}

static struct vt *mk(int cols, int rows, int hist)
{
	struct vt_callbacks cb = { .write = on_write, .title = on_title };
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
	CHECK(vt->pri[0][9].attr & ATTR_WRAP);
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
	struct cell *l = vt->pri[0];
	CHECK(l[0].fg == 1 && (l[0].attr & ATTR_BOLD) && (l[0].attr & ATTR_UNDERLINE));
	CHECK(l[1].fg == (COL_RGB | 0x010203));
	CHECK(l[2].fg == (COL_RGB | 0x040506));
	CHECK(l[3].bg == 100 && l[3].fg == COL_DEFAULT_FG);
	CHECK(l[4].fg == 200 && !(l[4].attr & (ATTR_BOLD | ATTR_UNDERLINE)));
	CHECK(l[5].fg == COL_DEFAULT_FG && l[5].bg == COL_DEFAULT_BG && !l[5].attr);
	CHECK((l[6].attr & ATTR_ITALIC) && l[6].fg == COL_DEFAULT_FG);
	CHECK(l[7].fg == 12 && l[7].bg == 11);
	/* background color erase */
	w(vt, "\033[44m\033[2;1H\033[K");
	CHECK(vt->pri[1][10].bg == 4);
	vt_free(vt);
}

static void test_wide(void)
{
	struct vt *vt = mk(6, 2, 0);
	w(vt, "a\xe7\x95\x8c" "b"); /* a 界 b */
	CHECK(vt->pri[0][1].cp == 0x754c && (vt->pri[0][1].attr & ATTR_WIDE));
	CHECK(vt->pri[0][2].attr & ATTR_WDUMMY);
	CHECK(vt->pri[0][3].cp == 'b' && vt->c.x == 4);
	/* overwrite right half: left half becomes blank */
	w(vt, "\033[1;3Hx");
	CHECK(vt->pri[0][1].cp == ' ' && !(vt->pri[0][1].attr & ATTR_WIDE));
	CHECK(vt->pri[0][2].cp == 'x');
	/* wide char at the last column wraps */
	w(vt, "\033[1;6H\xe7\x95\x8c");
	CHECK(vt->pri[1][0].cp == 0x754c && vt->c.y == 1 && vt->c.x == 2);
	vt_free(vt);
}

/* row 0 with W and d for the halves of a wide character */
static const char *shape(struct vt *vt)
{
	static char buf[64];
	int x = 0;
	for (; x < vt->cols && x < (int)sizeof buf - 1; x++) {
		const struct cell *l = &vt->line[0][x];
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

static void fuzz_write(void *ud, const char *b, size_t n) { (void)ud; (void)b; (void)n; }
static void fuzz_title(void *ud, const char *t) { (void)ud; (void)t; }

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
static const char *fuzz_broken_grid(struct cell **s, int cols, int rows)
{
	for (int r = 0; r < rows; r++)
		for (int x = 0; x < cols; x++) {
			unsigned a = s[r][x].attr;
			if ((a & ATTR_WIDE) && (x + 1 >= cols || !(s[r][x + 1].attr & ATTR_WDUMMY)))
				return "left half of a wide character without its right half";
			if ((a & ATTR_WDUMMY) && (x == 0 || !(s[r][x - 1].attr & ATTR_WIDE)))
				return "right half of a wide character without its left half";
		}
	return NULL;
}

static const char *fuzz_broken(const struct vt *vt)
{
	if (vt->c.x < 0 || vt->c.x >= vt->cols || vt->c.y < 0 || vt->c.y >= vt->rows)
		return "cursor outside the grid";
	if (vt->top < 0 || vt->bot >= vt->rows || vt->top > vt->bot)
		return "scroll region outside the grid";
	const char *why = fuzz_broken_grid(vt->pri, vt->cols, vt->rows);
	return why ? why : fuzz_broken_grid(vt->alt, vt->cols, vt->rows);
}

static bool fuzz_same(const struct vt *a, const struct vt *b)
{
	if (a->c.x != b->c.x || a->c.y != b->c.y || a->c.wrapnext != b->c.wrapnext ||
	    a->mode != b->mode || a->state != b->state || a->hist_count != b->hist_count)
		return false;
	for (int r = 0; r < a->rows; r++)
		for (int x = 0; x < a->cols; x++) {
			const struct cell *p = &a->line[r][x], *q = &b->line[r][x];
			if (p->cp != q->cp || p->fg != q->fg || p->bg != q->bg || p->attr != q->attr)
				return false;
		}
	return true;
}

static void fuzz_report(long seed, const char *what, const char *why)
{
	failures++;
	fprintf(stderr, "fuzz seed %ld: %s: %s\n", seed, what, why);
}

/* VT_FUZZ_SEEDS=100000 ./test_vt for a long run */
static void test_fuzz(void)
{
	const char *env = getenv("VT_FUZZ_SEEDS");
	long seeds = env ? atol(env) : 300;
	struct vt_callbacks cb = { .write = fuzz_write, .title = fuzz_title };
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
		struct vt *whole = vt_new(cols, rows, 50, &cb), *split = vt_new(cols, rows, 50, &cb);
		vt_write(whole, buf, n);
		for (size_t i = 0; i < n;) {
			size_t k = 1 + fuzz_rand() % 7;
			k = k > n - i ? n - i : k;
			vt_write(split, buf + i, k);
			i += k;
		}
		if (!fuzz_same(whole, split))
			fuzz_report(seed, "chunks", "the result depends on how the input is cut");
		vt_free(whole);
		vt_free(split);

		/* in pieces, with what a window does in between */
		struct vt *vt = vt_new(cols, rows, 50, &cb);
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
	w(vt, "\033[4h\033[4$p\033[20$p\033[99$p\033[4l");
	CHECK_STR(replybuf, "\033[4;1$y\033[20;2$y\033[99;0$y");
	/* a private mode with an intermediate is not a set or reset */
	w(vt, "\033[?2004$h\033[?2004$l");
	CHECK(vt->mode & MODE_BRACKETED);
	w(vt, "\033[?2004l");
	/* DCS strings are swallowed without output */
	w(vt, "\033P+q544e\033\\ok");
	CHECK_STR(row(vt, 2), "   ok");
	vt_free(vt);
}

static void test_charset_and_misc(void)
{
	struct vt *vt = mk(10, 3, 0);
	w(vt, "\033(0lqk\033(Bq");
	CHECK(vt->pri[0][0].cp == 0x250c && vt->pri[0][1].cp == 0x2500);
	CHECK(vt->pri[0][3].cp == 'q');
	w(vt, "\r\nab\033[3b");
	CHECK_STR(row(vt, 1), "abbbb");
	w(vt, "\033[1;1H\033[2P");
	CHECK(vt->pri[0][0].cp == 0x2510 && vt->pri[0][1].cp == 'q');
	w(vt, "\033[2;2H\033[2@");
	CHECK_STR(row(vt, 1), "a  bbbb");
	w(vt, "\033[4h\033[2;1HZ\033[4l");
	CHECK_STR(row(vt, 1), "Za  bbbb");
	w(vt, "\033[2;1H\033[3X");
	CHECK_STR(row(vt, 1), "    bbbb");
	/* tabs */
	w(vt, "\033[3;1H\tT\033[Z\033[Z");
	CHECK(vt->pri[2][8].cp == 'T' && vt->c.x == 0);
	/* split UTF-8 across writes */
	vt_write(vt, "\xe2\x82", 2);
	vt_write(vt, "\xac", 1);
	CHECK(vt->pri[2][0].cp == 0x20ac);
	/* invalid UTF-8 shows a replacement char */
	w(vt, "\xff");
	CHECK(vt->pri[2][1].cp == 0xfffd);
	/* unknown sequences are ignored cleanly */
	w(vt, "\033[?1234;5h\033[>4;2m\033[=5uX");
	CHECK(vt->pri[2][2].cp == 'X');
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
	vt_resize(vt, 12, 6);
	CHECK_STR(row(vt, 2), "5");
	CHECK_STR(row(vt, 5), "");
	w(vt, "\033[6;12Hx");
	CHECK(vt->pri[5][11].cp == 'x');
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
	CHECK(vt->pri[2][0].cp == 'o');
	CHECK(vt->pri[4][0].cp == 'p');
	w(vt, "\033[6n");
	CHECK_STR(replybuf, "\033[3;2R");
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
	/* sizes are clamped, the largest grid still works */
	struct vt *vt = mk(INT_MAX, INT_MAX, 0);
	CHECK(vt && vt->cols == VT_MAX_COLS && vt->rows == VT_MAX_ROWS);
	w(vt, "\033[2048;4096Hz");
	CHECK(vt->pri[VT_MAX_ROWS - 1][VT_MAX_COLS - 1].cp == 'z');
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
	test_scroll_region_ops();
	test_sgr();
	test_wide();
	test_wide_insert();
	test_altscreen();
	test_replies();
	test_charset_and_misc();
	test_resize();
	test_origin();
	test_cursor_style_redraw();
	test_alloc_failures();
	test_fuzz();
	printf("%d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
