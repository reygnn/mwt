/* test_link.c - URLs over the VT core's lines and the opener's words, headless:
 * make check */
#define _GNU_SOURCE
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "link.h"

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

static void nop_write(void *ud, const char *b, size_t n) { (void)ud; (void)b; (void)n; }

static struct vt *mk(int cols, int rows, int hist)
{
	struct vt_callbacks cb = { .write = nop_write };
	struct vt *vt = vt_new(cols, rows, hist, &cb);
	if (!vt) {
		fprintf(stderr, "test_link: no vt\n");
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
	return (int64_t)vt->scrolled - vt->view + r;
}

/* The URL at row r, column col, or NULL; its cells in *l. */
static void check_at(int ln, const struct vt *vt, int r, int col, const char *want,
		     struct link *l)
{
	char *got = NULL;
	struct link tmp;
	bool found = link_at(vt, line(vt, r), col, l ? l : &tmp, &got);
	checks++;
	if (found != (want != NULL) || (found && (!got || strcmp(got, want)))) {
		failures++;
		fprintf(stderr, "%s:%d: FAIL: at %d,%d \"%s\", expected \"%s\"\n", __FILE__, ln,
			r, col, found ? got : "(none)", want ? want : "(none)");
	}
	free(got);
}
#define AT(vt, r, col, want) check_at(__LINE__, vt, r, col, want, NULL)

/* The cells of the URL at row r, col: lines relative to the screen. */
#define CELLS(vt, r, col, br, bc_, er, ec_) do { struct link l_; \
	bool f_ = link_at(vt, line(vt, r), col, &l_, NULL); checks++; \
	if (!f_ || l_.ba != line(vt, br) || l_.bc != (bc_) || l_.ea != line(vt, er) || \
	    l_.ec != (ec_)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: link at %d,%d\n", __FILE__, __LINE__, r, col); } \
	} while (0)

static void test_schemes(void)
{
	struct vt *vt = mk(80, 6, 10);
	/*          0         1         2         3         4
	 *          01234567890123456789012345678901234567890123456789 */
	w(vt, "see https://example.com/a?b=c#d and more\r\n");
	AT(vt, 0, 4, "https://example.com/a?b=c#d");
	AT(vt, 0, 12, "https://example.com/a?b=c#d");
	AT(vt, 0, 30, "https://example.com/a?b=c#d");
	CELLS(vt, 0, 10, 0, 4, 0, 30);
	AT(vt, 0, 3, NULL);   /* the blank before */
	AT(vt, 0, 31, NULL);  /* and after */
	AT(vt, 0, 0, NULL);
	AT(vt, 0, 70, NULL);  /* past the text */
	w(vt, "HTTP://X.ORG ftp://f.org/p file:///etc/hosts mailto:a@b.c\r\n");
	AT(vt, 1, 2, "HTTP://X.ORG");
	AT(vt, 1, 15, "ftp://f.org/p");
	AT(vt, 1, 30, "file:///etc/hosts");
	AT(vt, 1, 50, "mailto:a@b.c");
	/* other schemes, a scheme alone, a scheme within a word */
	w(vt, "gopher://g.org ssh://h http:// mailto: xhttp://a.b 2http://c.d\r\n");
	AT(vt, 1, 0, "HTTP://X.ORG");
	AT(vt, 2, 3, NULL);
	AT(vt, 2, 17, NULL);
	AT(vt, 2, 25, NULL);
	AT(vt, 2, 33, NULL);
	AT(vt, 2, 42, NULL);
	AT(vt, 2, 55, NULL);
	/* a word before it with a mark between is not one */
	w(vt, "url:https://a.b/c x=http://d.e\r\n");
	AT(vt, 3, 0, NULL);   /* "url:" is not part of it */
	AT(vt, 3, 6, "https://a.b/c");
	AT(vt, 3, 25, "http://d.e");
	/* another scheme in its query is part of it, wherever the pointer is */
	w(vt, "http://a.b/?u=https://c.d/e x\r\n");
	AT(vt, 4, 2, "http://a.b/?u=https://c.d/e");
	AT(vt, 4, 20, "http://a.b/?u=https://c.d/e");
	vt_free(vt);

	/* CJK letters among the punctuation do not end it: 々, 〇, a fullwidth 1 */
	vt = mk(80, 2, 10);
	w(vt, "https://ja.wikipedia.org/wiki/\xe4\xbd\x90\xe3\x80\x85\xe6\x9c\xa8 "
	      "http://z.cn/\xe3\x80\x87\xef\xbc\x91");
	AT(vt, 0, 3, "https://ja.wikipedia.org/wiki/\xe4\xbd\x90\xe3\x80\x85\xe6\x9c\xa8");
	AT(vt, 0, 40, "http://z.cn/\xe3\x80\x87\xef\xbc\x91");
	vt_free(vt);
}

static void test_ends(void)
{
	struct vt *vt = mk(80, 12, 10);
	w(vt, "(see http://a.b/c). Or http://a.b/d, http://a.b/e!\r\n");
	AT(vt, 0, 8, "http://a.b/c");
	AT(vt, 0, 17, NULL);  /* the ) that closed what was before it */
	AT(vt, 0, 25, "http://a.b/d");
	AT(vt, 0, 40, "http://a.b/e");
	/* brackets it opened itself stay */
	w(vt, "http://w.org/wiki/Foo_(bar) [http://x.y/[1]] <http://l.m> \"http://q.r\"\r\n");
	AT(vt, 1, 3, "http://w.org/wiki/Foo_(bar)");
	AT(vt, 1, 30, "http://x.y/[1]");
	AT(vt, 1, 48, "http://l.m");
	AT(vt, 1, 62, "http://q.r");
	/* ends at the characters that end it */
	w(vt, "http://a.b/c|d http://a.b/e`f http://a.b/g^h http://a.b/i{j http://a.b/k\\l\r\n");
	AT(vt, 2, 0, "http://a.b/c");
	AT(vt, 2, 13, NULL);
	AT(vt, 2, 15, "http://a.b/e");
	AT(vt, 2, 30, "http://a.b/g");
	AT(vt, 2, 45, "http://a.b/i");
	AT(vt, 2, 60, "http://a.b/k");
	/* sentence marks at its end go, in the middle they stay */
	w(vt, "http://a.b/x.y;z?q=1:2... http://a.b/'quoted'\r\n");
	AT(vt, 3, 0, "http://a.b/x.y;z?q=1:2");
	AT(vt, 3, 23, NULL);
	AT(vt, 3, 30, "http://a.b/'quoted");

	/* a frame of box drawing next to it, as tmux draws */
	w(vt, "\xe2\x94\x82http://a.b/c\xe2\x94\x82 x\r\n");
	AT(vt, 4, 5, "http://a.b/c");
	CELLS(vt, 4, 5, 4, 1, 4, 12);
	/* letters of other scripts and wide characters are in it */
	w(vt, "http://b\xc3\xbc" "cher.de/\xe6\x97\xa5\xe6\x9c\xac x\r\n");
	AT(vt, 5, 3, "http://b\xc3\xbc" "cher.de/\xe6\x97\xa5\xe6\x9c\xac");
	/* on the right half of the last wide character */
	AT(vt, 5, 20, "http://b\xc3\xbc" "cher.de/\xe6\x97\xa5\xe6\x9c\xac");
	CELLS(vt, 5, 0, 5, 0, 5, 19);
	AT(vt, 5, 21, NULL);
	/* the pointer on the right half of a wide character within it */
	w(vt, "\xe6\x97\xa5 http://x.y/\xe6\x97\xa5\xe6\x9c\xac/z\r\n");
	AT(vt, 6, 14, "http://x.y/\xe6\x97\xa5\xe6\x9c\xac/z");
	AT(vt, 6, 15, "http://x.y/\xe6\x97\xa5\xe6\x9c\xac/z");
	AT(vt, 6, 1, NULL);
	/* hidden text ends it: what is seen is what opens */
	w(vt, "\033[10;1Hhttp://good.com\033[8m.evil.net\033[28m x\r\n");
	AT(vt, 9, 3, "http://good.com");
	AT(vt, 9, 17, NULL);
	AT(vt, 9, 15, NULL); /* on the first hidden cell */
	w(vt, "\033[11;1H\033[2Khttp://a.b\033[8mcd\033[28m x");
	AT(vt, 10, 10, NULL);
	AT(vt, 10, 3, "http://a.b");
	/* a hidden scheme before what is seen */
	w(vt, "\033[12;1H\033[2K\033[8mhttp\033[28m://x.y");
	AT(vt, 11, 6, NULL);
	w(vt, "\033[10;1H\033[2Khttp://good.com\033[8m.evil.net\033[28m x\r\n");
	/* CJK punctuation and fullwidth forms end it */
	w(vt, "http://example.jp\xe3\x80\x82x http://e.jp\xef\xbc\x89\r\n");
	AT(vt, 10, 3, "http://example.jp");
	AT(vt, 10, 25, "http://e.jp");
	w(vt, "\033[8;1H"); /* back to the rows below */
	/* a no-break space ends it, as a blank; a scheme with only marks after */
	w(vt, "http://a.b\xc2\xa0x http://. mailto:, http://)\r\n");
	AT(vt, 7, 3, "http://a.b");
	AT(vt, 7, 15, NULL);
	AT(vt, 7, 22, NULL);
	AT(vt, 7, 31, NULL);
	/* tabs and controls end it */
	w(vt, "http://a.b/c\thttp://d.e/f\r\n");
	AT(vt, 8, 2, "http://a.b/c");
	AT(vt, 8, 17, "http://d.e/f");
	vt_free(vt);
}

static void test_wrap(void)
{
	/* a URL over three lines that wrap, from the middle of the first */
	struct vt *vt = mk(10, 6, 10);
	w(vt, "ab http://abcdefghij.org/x y");
	/* ab http:// | abcdefghij | .org/x y */
	const char *u = "http://abcdefghij.org/x";
	AT(vt, 0, 3, u);
	AT(vt, 1, 5, u);
	AT(vt, 2, 4, u);
	CELLS(vt, 1, 0, 0, 3, 2, 5);
	AT(vt, 2, 6, NULL);
	AT(vt, 2, 7, NULL);
	AT(vt, 0, 1, NULL);
	/* lines that do not wrap are not joined: the next line is a new one */
	w(vt, "\r\nhttp://k.l\r\nmnop\r\n");
	AT(vt, 3, 2, "http://k.l");
	AT(vt, 4, 1, NULL);
	vt_free(vt);

	/* into the history and out of it, and with the view scrolled back */
	vt = mk(10, 3, 20);
	w(vt, "http://0123456789abcdefghij.org");
	w(vt, "\r\n1\r\n2\r\n3\r\n4");
	/* the URL's lines are in the history now */
	int64_t first = vt_abs_first(vt);
	struct link l;
	char *got = NULL;
	CHECK(link_at(vt, first + 1, 3, &l, &got));
	CHECK(got && !strcmp(got, "http://0123456789abcdefghij.org"));
	CHECK(l.ba == first && l.bc == 0 && l.ea == first + 3 && l.ec == 0);
	free(got);
	vt_scroll_view(vt, 5);
	AT(vt, 0, 0, "http://0123456789abcdefghij.org");
	vt_free(vt);

	/* a scheme cut by the wrap */
	vt = mk(10, 3, 10);
	w(vt, "123456 htt");
	w(vt, "p://a.b/c");
	AT(vt, 0, 8, "http://a.b/c");
	AT(vt, 1, 3, "http://a.b/c");
	vt_free(vt);

	/* The history's last line wraps, the screen after it does not go on with
	 * it: in the alternate screen, which has its own lines, and once the main
	 * screen is cleared from its start or reset. Else the URL would take a
	 * scheme from text no longer shown. */
	static const char *const after[] = {
		"\033[?1049h\033[H", "\033[2J\033[H", "\033[H\033[J", "\033[3;1H\033[1J\033[H",
		"\033c", "\033[?1049h\033c",
	};
	for (size_t i = 0; i < sizeof after / sizeof *after; i++) {
		vt = mk(10, 3, 10);
		w(vt, "12345 http://abcdefghijklmnopqrstu");
		CHECK(vt->hist_count > 0);
		w(vt, after[i]);
		w(vt, "://evil.o");
		AT(vt, 0, 3, NULL);
		vt_free(vt);
	}
	/* a line erased is not a screen cleared: readline erases the line it
	 * redraws, which still goes on from the one above */
	vt = mk(10, 3, 10);
	w(vt, "12345 http://abcdefghijklmnopqrstu");
	w(vt, "\033[H\033[2K://evil.o");
	AT(vt, 0, 3, "http://evil.o");
	vt_free(vt);
	/* erased from elsewhere than its start, the screen still goes on from the
	 * history */
	vt = mk(10, 3, 10);
	w(vt, "12345 http://abcdefghijklmnopqrstu");
	w(vt, "\033[3;1H\033[J");
	AT(vt, 0, 3, "http://abcdefghijklmnopq");
	vt_free(vt);
	/* a window grown wider: the rows of the main screen are laid out anew, on
	 * the screen as in the history */
	vt = mk(10, 4, 10);
	w(vt, "http://example.com/abcdef");
	CHECK(vt_resize(vt, 20, 4));
	AT(vt, 0, 2, "http://example.com/abcdef");
	AT(vt, 0, 12, "http://example.com/abcdef");
	AT(vt, 1, 3, "http://example.com/abcdef");
	AT(vt, 1, 5, NULL);
	AT(vt, 2, 4, NULL);
	w(vt, "\r\n\r\n\r\n\r\n");
	struct link lw;
	char *gw = NULL;
	CHECK(link_at(vt, vt_abs_first(vt) + 1, 3, &lw, &gw) && gw &&
	      !strcmp(gw, "http://example.com/abcdef"));
	free(gw);
	vt_free(vt);
	/* on the alternate screen, which is not, the flag stays where the line
	 * ended, blanks after it */
	vt = mk(10, 4, 10);
	w(vt, "\033[?47hhttp://example.com/abcdef");
	CHECK(vt_resize(vt, 20, 4));
	AT(vt, 0, 2, "http://example.com/abcdef");
	AT(vt, 1, 3, "http://example.com/abcdef");
	AT(vt, 2, 4, "http://example.com/abcdef");
	AT(vt, 0, 12, NULL); /* the blanks after the wrap */
	vt_free(vt);
	/* a wide glyph that did not fit at the end of a row does not end the URL:
	 * the cell left over is not in it */
	vt = mk(10, 4, 10);
	w(vt, "http://a/\xe6\xbc\xa2\xe6\xbc\xa2 x");
	AT(vt, 0, 2, "http://a/\xe6\xbc\xa2\xe6\xbc\xa2");
	AT(vt, 1, 2, "http://a/\xe6\xbc\xa2\xe6\xbc\xa2");
	AT(vt, 0, 9, NULL);
	vt_free(vt);
	/* the alternate screen cleared leaves the main screen's lines as they are */
	vt = mk(10, 3, 10);
	w(vt, "12345 http://abcdefghijklmnopqrstu");
	w(vt, "\033[?1049h\033[2J\033[?1049l");
	AT(vt, 0, 3, "http://abcdefghijklmnopqrstu");
	vt_free(vt);
}

/* A URL longer than LINK_SPAN either way is cut there, and never read past. */
static void test_long(void)
{
	/* a scheme right where LINK_SPAN cuts the line before: it may be in the
	 * middle of a word, so it is none; from closer by, it is in one */
	{
		struct vt *vt = mk(100, 4, 200);
		char *s = malloc(4301);
		memset(s, 'a', 100);
		memcpy(s + 100, "http://", 7);
		memset(s + 107, 'b', 4300 - 107);
		s[4300] = '\0';
		w(vt, s);
		w(vt, "\r\n");
		int64_t first = vt_abs_first(vt);
		struct link l;
		CHECK(!link_at(vt, first + 41, 99, &l, NULL));
		CHECK(!link_at(vt, first + 40, 99, &l, NULL));
		CHECK(!link_at(vt, first + 1, 50, &l, NULL));
		free(s);
		vt_free(vt);
	}
	struct vt *vt = mk(100, 4, 400);
	size_t n = 3 * LINK_SPAN;
	char *s = malloc(n + 1);
	memcpy(s, "http://", 7);
	for (size_t i = 7; i < n; i++)
		s[i] = (char)('a' + i % 26);
	s[n] = '\0';
	w(vt, s);
	w(vt, "\r\n");
	int64_t first = vt_abs_first(vt);
	/* from the first line: its cells and LINK_SPAN after, to the end of a line */
	struct link l;
	char *got = NULL;
	CHECK(link_at(vt, first, 50, &l, &got));
	CHECK(got && strlen(got) >= LINK_SPAN && strlen(got) <= LINK_SPAN + 200 &&
	      !strncmp(got, s, strlen(got)));
	free(got);
	/* from the middle the scheme is too far back */
	CHECK(!link_at(vt, first + 2 * LINK_SPAN / 100, 50, &l, NULL));
	free(s);
	vt_free(vt);
}

/* Thousands of brackets it did not open, trimmed in one pass. */
static void test_brackets(void)
{
	struct vt *vt = mk(200, 60, 10);
	char *s = malloc(10100);
	memcpy(s, "http://a/(b)", 12);
	memset(s + 12, ')', 10000);
	s[10012] = '\0';
	w(vt, s);
	CELLS(vt, 0, 5, 0, 0, 0, 11);
	AT(vt, 20, 5, NULL);
	free(s);
	vt_free(vt);
}

/* Cut by LINK_SPAN before it: the run's start is not seen, so neither a scheme
 * in its query nor its own counts. */
static void test_cut(void)
{
	struct vt *vt = mk(100, 4, 200);
	char *s = malloc(5000);
	int n = sprintf(s, "https://tracker.example/r?");
	memset(s + n, 'a', 4474);
	n += 4474;
	n += sprintf(s + n, "&u=https://target.example/x\r\n");
	w(vt, s);
	int64_t first = vt_abs_first(vt);
	struct link l;
	char *got = NULL;
	/* near its start: the long one, cut */
	CHECK(link_at(vt, first, 10, &l, &got) && got && !strncmp(got, "https://tracker", 15));
	free(got);
	/* near its end: none, not the inner one */
	int64_t last = first + (n - 2) / 100;
	CHECK(!link_at(vt, last, (n - 4) % 100, &l, NULL));
	free(s);
	vt_free(vt);

	/* the pointer past the text of a short history line */
	vt = mk(80, 2, 10);
	w(vt, "ab\r\n\r\n\r\n");
	CHECK(!link_at(vt, vt_abs_first(vt), 40, &l, NULL));
	vt_free(vt);
}

static void test_same(void)
{
	struct link a = { 1, 2, 3, 4 }, b = a;
	CHECK(link_same(&a, &b));
	b.ec++;
	CHECK(!link_same(&a, &b));
	b = a;
	b.ba++;
	CHECK(!link_same(&a, &b));
	b = a;
	b.bc--;
	CHECK(!link_same(&a, &b));
	b = a;
	b.ea++;
	CHECK(!link_same(&a, &b));
}

/* Each allocation fails once in turn: no URL then, nothing leaks, ASan sees. */
static void test_nomem(void)
{
	struct vt *vt = mk(20, 4, 10);
	w(vt, "x http://abcdefghijklmnopqrstuvwxyz.org y");
	for (long k = 0;; k++) {
		char *got = NULL;
		struct link l;
		injected = false;
		countdown = k;
		bool found = link_at(vt, line(vt, 1), 5, &l, &got);
		countdown = -1;
		if (!injected) {
			CHECK(found && got && !strcmp(got, "http://abcdefghijklmnopqrstuvwxyz.org"));
			free(got);
			CHECK(k > 1); /* the cells, then the text: both were failed */
			break;
		}
		CHECK(!found && !got);
	}
	vt_free(vt);
}

static void test_argv(void)
{
	char buf[64], *argv[8];
	CHECK(link_argv("xdg-open", "http://a.b", buf, sizeof buf, argv, 8) == 2);
	CHECK(!strcmp(argv[0], "xdg-open") && !strcmp(argv[1], "http://a.b") && !argv[2]);
	CHECK(link_argv("  firefox \t --new-tab  ", "u", buf, sizeof buf, argv, 8) == 3);
	CHECK(!strcmp(argv[0], "firefox") && !strcmp(argv[1], "--new-tab") &&
	      !strcmp(argv[2], "u") && !argv[3]);
	/* no shell: quotes and ; are only characters */
	CHECK(link_argv("sh -c 'x;y'", "u", buf, sizeof buf, argv, 8) == 4);
	CHECK(!strcmp(argv[2], "'x;y'"));
	/* nothing, too long, too many words; 6 words and the url need 8 */
	CHECK(link_argv("", "u", buf, sizeof buf, argv, 8) == 0);
	CHECK(link_argv(" \t ", "u", buf, sizeof buf, argv, 8) == 0);
	CHECK(link_argv("abcdefgh", "u", buf, 8, argv, 8) == 0);
	CHECK(link_argv("abcdefg", "u", buf, 8, argv, 8) == 2);
	CHECK(link_argv("a b c d e f", "u", buf, sizeof buf, argv, 8) == 7 && !argv[7]);
	CHECK(link_argv("a b c d e f g", "u", buf, sizeof buf, argv, 8) == 0);
}

int main(void)
{
	if (!setlocale(LC_CTYPE, "C.UTF-8"))
		setlocale(LC_CTYPE, "");
	test_schemes();
	test_ends();
	test_wrap();
	test_long();
	test_brackets();
	test_cut();
	test_same();
	test_nomem();
	test_argv();
	printf("test_link: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
