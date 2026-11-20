/* test_paste.c - what a paste becomes and the clipboard's Latin-1, headless:
 * make check */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "paste.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* A malloc and realloc that fail once, the countdown-th call from now, and
 * otherwise are the real ones. */
void *__real_malloc(size_t n);
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
void *__wrap_realloc(void *p, size_t n) { return inject() ? NULL : __real_realloc(p, n); }

static uint64_t rng = 0x9e3779b97f4a7c15u;
static unsigned rnd(unsigned n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned)(rng % n);
}

/* bytes as C escapes, for the failure message */
static const char *esc(const char *s, size_t n)
{
	static char out[4][512];
	static int k;
	char *o = out[k++ % 4];
	size_t j = 0;
	for (size_t i = 0; i < n && j < 500; i++) {
		unsigned char c = (unsigned char)s[i];
		j += (size_t)snprintf(o + j, 512 - j, c >= 0x20 && c < 0x7f && c != '\\' ?
				      "%c" : "\\x%02x", c);
	}
	o[j] = 0;
	return o;
}

static void check_bytes(int line, const char *got, size_t n, const char *want, size_t wn)
{
	checks++;
	if (n == wn && (!n || !memcmp(got, want, n)))
		return;
	failures++;
	fprintf(stderr, "%s:%d: FAIL: \"%s\", expected \"%s\"\n", __FILE__, line,
		esc(got, n), esc(want, wn));
}

/* The reference: what a decoder that follows RFC 3629 to the letter reads. */
static int ref_seq(const unsigned char *s, size_t n)
{
	int k;
	uint32_t cp, min;
	if (s[0] < 0x80)
		return 1;
	else if ((s[0] & 0xe0) == 0xc0) { k = 2; cp = s[0] & 0x1f; min = 0x80; }
	else if ((s[0] & 0xf0) == 0xe0) { k = 3; cp = s[0] & 0x0f; min = 0x800; }
	else if ((s[0] & 0xf8) == 0xf0) { k = 4; cp = s[0] & 0x07; min = 0x10000; }
	else return 0;
	if ((size_t)k > n)
		return 0;
	for (int i = 1; i < k; i++) {
		if ((s[i] & 0xc0) != 0x80)
			return 0;
		cp = cp << 6 | (s[i] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
		return 0;
	return k;
}

static uint32_t decode(const unsigned char *s, int k)
{
	uint32_t cp = k == 1 ? s[0] : s[0] & (0x3f >> (k - 1));
	for (int i = 1; i < k; i++)
		cp = cp << 6 | (s[i] & 0x3f);
	return cp;
}

static void test_utf8_seq(void)
{
	static const struct { const char *s; size_t n; int k; } cases[] = {
		{ "a", 1, 1 }, { "\xc3\xa9", 2, 2 }, { "\xe2\x82\xac", 3, 3 },
		{ "\xf0\x9f\x98\x80", 4, 4 },
		/* cut off */
		{ "\xc3", 1, 0 }, { "\xe2\x82", 2, 0 }, { "\xf0\x9f\x98", 3, 0 },
		/* no continuation */
		{ "\xc3\x28", 2, 0 }, { "\xe2\x28\xac", 3, 0 }, { "\xe2\x82\xc0", 3, 0 },
		/* overlong */
		{ "\xc0\x80", 2, 0 }, { "\xc1\xbf", 2, 0 }, { "\xe0\x80\x80", 3, 0 },
		{ "\xe0\x9f\xbf", 3, 0 }, { "\xe0\xa0\x80", 3, 3 },
		{ "\xf0\x80\x80\x80", 4, 0 }, { "\xf0\x8f\xbf\xbf", 4, 0 },
		{ "\xf0\x90\x80\x80", 4, 4 },
		/* surrogates */
		{ "\xed\xa0\x80", 3, 0 }, { "\xed\xbf\xbf", 3, 0 }, { "\xed\x9f\xbf", 3, 3 },
		{ "\xee\x80\x80", 3, 3 },
		/* past U+10FFFF */
		{ "\xf4\x8f\xbf\xbf", 4, 4 }, { "\xf4\x90\x80\x80", 4, 0 },
		{ "\xf5\x80\x80\x80", 4, 0 }, { "\xf8\x88\x80\x80\x80", 5, 0 },
		/* a continuation byte or one never used */
		{ "\x80", 1, 0 }, { "\xbf", 1, 0 }, { "\xfe", 1, 0 }, { "\xff", 1, 0 },
		/* only the first sequence counts */
		{ "ab", 2, 1 }, { "\xc3\xa9\xc3", 3, 2 },
	};
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		int k = utf8_seq((const unsigned char *)cases[i].s, cases[i].n);
		checks++;
		if (k != cases[i].k) {
			failures++;
			fprintf(stderr, "FAIL: utf8_seq(\"%s\") is %d, expected %d\n",
				esc(cases[i].s, cases[i].n), k, cases[i].k);
		}
	}

	/* every sequence of up to three bytes, against the reference */
	long wrong = 0;
	unsigned char b[4];
	for (unsigned v = 0; v < 1u << 24; v++) {
		b[0] = v >> 16; b[1] = v >> 8; b[2] = v;
		for (size_t n = 1; n <= 3; n++)
			wrong += utf8_seq(b, n) != ref_seq(b, n);
	}
	/* four bytes: every first two, the last two at the edges of continuation */
	static const unsigned char edge[] = { 0x00, 0x7f, 0x80, 0x8f, 0x90, 0xbf, 0xc0, 0xff };
	for (unsigned v = 0; v < 1u << 16; v++)
		for (size_t i = 0; i < sizeof edge; i++)
			for (size_t j = 0; j < sizeof edge; j++) {
				b[0] = v >> 8; b[1] = v; b[2] = edge[i]; b[3] = edge[j];
				wrong += utf8_seq(b, 4) != ref_seq(b, 4);
			}
	CHECK(wrong == 0);
}

static void test_put_utf8(void)
{
	/* every code point there is, and back */
	long wrong = 0;
	for (uint32_t cp = 0; cp <= 0x10ffff; cp++) {
		char *buf = NULL;
		size_t len = 0, cap = 0;
		if (!put_utf8(&buf, &len, &cap, cp)) {
			wrong++;
			continue;
		}
		int want = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
		int k = utf8_seq((unsigned char *)buf, len);
		bool surrogate = cp >= 0xd800 && cp <= 0xdfff;
		if ((int)len != want || (surrogate ? k != 0 :
		    k != want || decode((unsigned char *)buf, k) != cp))
			wrong++;
		free(buf);
	}
	CHECK(wrong == 0);

	/* growing: the text stays whole */
	char *buf = NULL;
	size_t len = 0, cap = 0;
	bool all = true;
	for (int i = 0; i < 10000; i++)
		all &= put_utf8(&buf, &len, &cap, 0x20ac);
	CHECK(all && len == 30000 && cap >= len);
	bool whole = true;
	for (size_t i = 0; i < len; i += 3)
		whole &= !memcmp(buf + i, "\xe2\x82\xac", 3);
	CHECK(whole);
	free(buf);

	/* out of memory: false, and the buffer as it was */
	buf = NULL;
	len = cap = 0;
	for (int i = 0; i < 300; i++)
		put_utf8(&buf, &len, &cap, 'x');
	while (len + 5 <= cap) /* up to the next growth */
		put_utf8(&buf, &len, &cap, 'x');
	char *was = buf;
	size_t was_len = len, was_cap = cap;
	countdown = 0;
	injected = false;
	CHECK(!put_utf8(&buf, &len, &cap, 0x1f600));
	countdown = -1;
	CHECK(injected && buf == was && len == was_len && cap == was_cap);
	CHECK(put_utf8(&buf, &len, &cap, 0x1f600) && len == was_len + 4);
	free(buf);
}

/* paste_decode of a literal; *r gets its result */
static char *dec(const char *in, size_t n, bool latin1, size_t max, size_t *len, int *r)
{
	char *out = (char *)"untouched";
	*r = paste_decode(in, n, latin1, max, &out, len);
	return out;
}
#define DECODE(in, latin1, want) do { \
	size_t len_; int r_; \
	char *o_ = dec(in, sizeof in - 1, latin1, 1 << 20, &len_, &r_); \
	CHECK(r_ == PASTE_OK); \
	if (r_ == PASTE_OK) { \
		check_bytes(__LINE__, o_ ? o_ : "", o_ ? len_ : 0, want, sizeof want - 1); \
		free(o_); \
	} \
} while (0)

static void test_decode(void)
{
	DECODE("abc", false, "abc");
	DECODE("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80", false, "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80");
	/* STRING is Latin-1 */
	DECODE("caf\xe9", true, "caf\xc3\xa9");
	DECODE("\xc3\xa9", true, "\xc3\x83\xc2\xa9");
	DECODE("\xff", true, "\xc3\xbf");
	/* and so is what is not UTF-8, byte by byte, whatever the label */
	DECODE("\xe9t\xe9", false, "\xc3\xa9t\xc3\xa9");
	DECODE("\xc3", false, "\xc3\x83");
	DECODE("\xc0\xaf", false, "\xc3\x80\xc2\xaf");
	DECODE("\xed\xa0\x80", false, "\xc3\xad\xc2\xa0"); /* a surrogate; 0x80 is C1 */
	/* C1 controls go, as UTF-8 and as Latin-1; U+00A0 stays */
	DECODE("a\xc2\x85" "b\xc2\x9f" "c\xc2\xa0", false, "abc\xc2\xa0");
	DECODE("a\x85" "b\x9f" "c\xa0", true, "abc\xc2\xa0");
	DECODE("\x80\x9f", false, "");
	/* C0 controls stay here, paste_filter takes them out */
	DECODE("\x03\x1b\x7f\t", false, "\x03\x1b\x7f\t");
	/* a NUL is a character like any other */
	DECODE("a\0b", false, "a\0b");

	/* nothing: no text, and no buffer */
	size_t len;
	int r;
	char *o = dec("", 0, false, 10, &len, &r);
	CHECK(r == PASTE_OK && o == NULL && len == 0);
	o = dec("\x85\x85", 2, true, 10, &len, &r);
	CHECK(r == PASTE_OK && o == NULL && len == 0);

	/* the limit is on what comes out: Latin-1 doubles */
	o = dec("abcd", 4, false, 4, &len, &r);
	CHECK(r == PASTE_OK && len == 4);
	free(o);
	o = dec("abcde", 5, false, 4, &len, &r);
	CHECK(r == PASTE_TOO_LARGE);
	o = dec("\xe9\xe9", 2, true, 4, &len, &r);
	CHECK(r == PASTE_OK && len == 4);
	free(o);
	o = dec("\xe9\xe9", 2, true, 3, &len, &r);
	CHECK(r == PASTE_TOO_LARGE);
	o = dec("\x85\x85\x85\x85\x85", 5, true, 4, &len, &r);
	CHECK(r == PASTE_OK && len == 0);

	/* out of memory at each allocation in turn: PASTE_NOMEM, nothing leaks */
	static char big[5000];
	for (size_t i = 0; i < sizeof big; i++)
		big[i] = (char)(0xa0 + i % 0x60);
	int seen = 0;
	for (long n = 0;; n++) {
		countdown = n;
		injected = false;
		r = paste_decode(big, sizeof big, true, 1 << 20, &o, &len);
		countdown = -1;
		if (!injected) {
			CHECK(r == PASTE_OK && len == 2 * sizeof big);
			free(o);
			break;
		}
		seen++;
		CHECK(r == PASTE_NOMEM);
	}
	CHECK(seen >= 5); /* 256 bytes doubling to 10000 */

	/* random bytes: valid UTF-8 out, no C1 controls, every character kept */
	long bad = 0;
	for (int round = 0; round < 3000; round++) {
		char in[64];
		size_t n = rnd(sizeof in);
		for (size_t i = 0; i < n; i++)
			in[i] = (char)(rnd(4) ? (char)rnd(256) : "\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"[rnd(9)]);
		bool latin1 = rnd(2);
		r = paste_decode(in, n, latin1, 1 << 20, &o, &len);
		if (r != PASTE_OK) {
			bad++;
			continue;
		}
		size_t chars = 0, kept = 0;
		for (size_t i = 0; i < len;) {
			int k = utf8_seq((unsigned char *)o + i, len - i);
			if (!k) {
				bad++;
				break;
			}
			uint32_t cp = decode((unsigned char *)o + i, k);
			if (cp >= 0x80 && cp < 0xa0)
				bad++;
			i += k;
			kept++;
		}
		/* each input character, or byte where it is none, is one out or a C1 */
		for (size_t i = 0; i < n; chars++) {
			int k = latin1 ? 0 : utf8_seq((unsigned char *)in + i, n - i);
			uint32_t cp = k > 1 ? decode((unsigned char *)in + i, k) : (unsigned char)in[i];
			i += k ? (size_t)k : 1;
			if (cp >= 0x80 && cp < 0xa0)
				chars--;
		}
		if (kept != chars)
			bad++;
		free(o);
	}
	CHECK(bad == 0);
}

#define FILTER(in, br, want) do { \
	char *out_ = malloc(sizeof in - 1 + 12); \
	size_t n_ = paste_filter(in, sizeof in - 1, br, out_); \
	check_bytes(__LINE__, out_, n_, want, sizeof want - 1); \
	free(out_); \
} while (0)

static void test_filter(void)
{
	FILTER("ab", false, "ab");
	FILTER("", false, "");
	/* lines end in CR, as the Enter key sends */
	FILTER("a\nb", false, "a\rb");
	FILTER("a\r\nb", false, "a\rb");
	FILTER("a\rb", false, "a\rb");
	FILTER("\n", false, "\r");
	FILTER("\n\n", false, "\r\r");
	FILTER("\r\n\r\n", false, "\r\r");
	FILTER("\r\r\n", false, "\r\r");
	FILTER("\n\r", false, "\r\r");
	/* of the controls only tab and CR stay */
	FILTER("a\tb", false, "a\tb");
	FILTER("x\x03y\x04z\x1a", false, "xyz");
	FILTER("\x15\x17\x7f\b", false, "");
	FILTER("a\0b", false, "ab");
	FILTER("\033[201~rm -rf ~\r", false, "[201~rm -rf ~\r");
	/* text beyond ASCII goes through */
	FILTER("\xc3\xa9\xe2\x82\xac", false, "\xc3\xa9\xe2\x82\xac");
	/* bracketed paste: the brackets, and no ESC between them */
	FILTER("x", true, "\033[200~x\033[201~");
	FILTER("", true, "\033[200~\033[201~");
	FILTER("\033[201~\n", true, "\033[200~[201~\r\033[201~");

	/* random text: out of bounds is ASan's, the bytes are checked here */
	long bad = 0;
	for (int round = 0; round < 3000; round++) {
		size_t n = rnd(80);
		char *in = malloc(n + 1), *out = malloc(n + 12);
		for (size_t i = 0; i < n; i++)
			in[i] = (char)(rnd(3) ? (char)rnd(256) : "\r\n\t\033\x03\x7f"[rnd(6)]);
		bool br = rnd(2);
		size_t o = paste_filter(in, n, br, out);
		size_t a = br ? 6 : 0, b = br ? o - 6 : o;
		if (o > n + 12 || (br && (memcmp(out, "\033[200~", 6) ||
					  memcmp(out + o - 6, "\033[201~", 6))))
			bad++;
		for (size_t i = a; i < b; i++) {
			unsigned char c = (unsigned char)out[i];
			if ((c < 0x20 && c != '\t' && c != '\r') || c == 0x7f)
				bad++;
		}
		/* as many CR as line ends: CR, LF and CR LF each one */
		size_t ends = 0, crs = 0;
		for (size_t i = 0; i < n; i++)
			ends += in[i] == '\r' || (in[i] == '\n' && (i == 0 || in[i - 1] != '\r'));
		for (size_t i = a; i < b; i++)
			crs += out[i] == '\r';
		if (ends != crs)
			bad++;
		free(in);
		free(out);
	}
	CHECK(bad == 0);
}

static struct brackets feed(const char *s, size_t n)
{
	struct brackets b = { 0, false };
	track_brackets(&b, s, n);
	return b;
}

static void test_brackets(void)
{
	CHECK(feed("\033[200~", 6).open);
	CHECK(!feed("\033[200~x\033[201~", 13).open);
	CHECK(!feed("\033[201~", 6).open);
	CHECK(!feed("", 0).open && feed("", 0).seen == 0);
	/* how far a bracket got */
	CHECK(feed("\033", 1).seen == 1);
	CHECK(feed("\033[20", 4).seen == 4);
	CHECK(feed("\033[200", 5).seen == 5);
	CHECK(feed("\033[201", 5).seen == 6);
	CHECK(feed("\033[200~", 6).seen == 0);
	/* what only looks alike */
	CHECK(!feed("\033[202~", 6).open);
	CHECK(!feed("\033[20~", 5).open);
	CHECK(!feed("\033[2000~", 7).open);
	CHECK(!feed("[200~", 5).open);
	CHECK(!feed("~~~", 3).open);
	/* an ESC starts over, also halfway through a bracket */
	CHECK(feed("\033[2\033[200~", 9).open);
	CHECK(feed("\033\033[200~", 7).open);
	/* the paste's own text does not change it, nor a ~ in it */
	CHECK(feed("\033[200~a~b[20~c", 14).open);

	/* cut anywhere, in any number of writes: as in one */
	static const char *streams[] = {
		"\033[200~hello\r\033[201~", "\033[200~a~\033[201~\033[200~b", "x\033[200~",
		"\033[A\033[200~\033[201~\033[B", "\033[201~\033[200~\033[2",
	};
	long bad = 0;
	for (size_t s = 0; s < sizeof streams / sizeof streams[0]; s++) {
		const char *str = streams[s];
		size_t n = strlen(str);
		struct brackets whole = feed(str, n);
		for (int round = 0; round < 500; round++) {
			struct brackets b = { 0, false };
			for (size_t i = 0; i < n;) {
				size_t k = 1 + rnd((unsigned)(n - i));
				track_brackets(&b, str + i, k);
				i += k;
			}
			bad += b.open != whole.open || b.seen != whole.seen;
		}
	}
	CHECK(bad == 0);
	/* what paste_filter writes, after random keys: open after a cut-off paste */
	bad = 0;
	for (int round = 0; round < 2000; round++) {
		char in[40], out[40 + 12], all[200];
		size_t n = rnd(sizeof in), m = 0;
		for (size_t i = 0; i < n; i++)
			in[i] = (char)rnd(256);
		size_t keys = rnd(20);
		for (size_t i = 0; i < keys; i++)
			all[m++] = rnd(4) ? (char)(0x20 + rnd(0x5f)) : "\033[A"[rnd(3)];
		size_t o = paste_filter(in, n, true, out);
		size_t cut = rnd((unsigned)o + 1);
		memcpy(all + m, out, cut);
		m += cut;
		struct brackets b = { 0, false };
		for (size_t i = 0; i < m;) {
			size_t k = 1 + rnd((unsigned)(m - i));
			track_brackets(&b, all + i, k);
			i += k;
		}
		bad += b.open != (cut >= 6 && cut < o);
	}
	CHECK(bad == 0);
}

static void test_latin1(void)
{
	char *s = to_latin1("abc");
	CHECK(s && !strcmp(s, "abc"));
	free(s);
	s = to_latin1("caf\xc3\xa9 \xc2\xa0\xc3\xbf");
	CHECK(s && !strcmp(s, "caf\xe9 \xa0\xff"));
	free(s);
	/* what Latin-1 cannot hold, and the C1 controls */
	s = to_latin1("\xe2\x82\xac \xf0\x9f\x98\x80 \xc2\x85 \xc4\x80");
	CHECK(s && !strcmp(s, "? ? ? ?"));
	free(s);
	s = to_latin1("");
	CHECK(s && !*s);
	free(s);
	countdown = 0;
	s = to_latin1("abc");
	countdown = -1;
	CHECK(!s);

	/* every code point, as put_utf8 writes it */
	long wrong = 0;
	for (uint32_t cp = 1; cp <= 0x10ffff; cp++) {
		if (cp >= 0xd800 && cp <= 0xdfff)
			continue;
		char *buf = NULL;
		size_t len = 0, cap = 0;
		put_utf8(&buf, &len, &cap, cp);
		put_utf8(&buf, &len, &cap, 0);
		char *l = to_latin1(buf);
		char want = cp < 0x80 || (cp >= 0xa0 && cp < 0x100) ? (char)cp : '?';
		if (!l || l[0] != want || l[1])
			wrong++;
		free(l);
		free(buf);
	}
	CHECK(wrong == 0);
	/* not UTF-8, which mwt's own selection never is: no read past the end */
	s = to_latin1("\xc3");
	CHECK(s && strlen(s) == 1);
	free(s);
	s = to_latin1("\xf0\x9f");
	CHECK(s && strlen(s) == 1);
	free(s);
}

static void test_mime(void)
{
	/* best first: UTF-8 under its names, then text/plain, then Latin-1 */
	CHECK(mime_rank("text/plain;charset=utf-8") == 0);
	CHECK(mime_rank("UTF8_STRING") == 1);
	CHECK(mime_rank("text/plain") == 2);
	CHECK(mime_rank("TEXT") == 3);
	CHECK(mime_rank("STRING") == 4 && !strcmp(mimes[4], "STRING"));
	for (int i = 0; i < MIME_COUNT; i++)
		CHECK(mime_rank(mimes[i]) == i);
	/* exactly as named */
	CHECK(mime_rank("text/html") == MIME_COUNT);
	CHECK(mime_rank("text/plain;charset=UTF-8") == MIME_COUNT);
	CHECK(mime_rank("text/plain;charset=utf-8 ") == MIME_COUNT);
	CHECK(mime_rank("utf8_string") == MIME_COUNT);
	CHECK(mime_rank("") == MIME_COUNT);
}

/* n bytes of x through a pipe, read with max and turn; the result and the bytes
 * read. The pipe is non-blocking, as the clipboard's in mwt. */
static int read_n(size_t n, size_t max, size_t turn, size_t *got, bool close_it, long fail)
{
	int fd[2];
	if (pipe(fd) < 0)
		return -1;
	fcntl(fd[0], F_SETFL, O_NONBLOCK);
	fcntl(fd[1], F_SETPIPE_SZ, 1 << 20);
	char *w = malloc(n ? n : 1);
	memset(w, 'x', n);
	if (write(fd[1], w, n) != (ssize_t)n)
		n = 0;
	free(w);
	if (close_it)
		close(fd[1]);
	char *buf = NULL;
	size_t len = 0, cap = 0;
	countdown = fail;
	int r = paste_read(fd[0], &buf, &len, &cap, max, turn);
	countdown = -1;
	*got = len;
	free(buf);
	close(fd[0]);
	if (!close_it)
		close(fd[1]);
	return r;
}

static void test_read(void)
{
	size_t got;
	countdown = -1;
	/* exactly max fits; one byte more is too large: mwt took 64 MiB less 4 KiB */
	CHECK(read_n(10000, 10000, 1 << 20, &got, true, -1) == PASTE_OK && got == 10000);
	CHECK(read_n(10001, 10000, 1 << 20, &got, true, -1) == PASTE_TOO_LARGE);
	CHECK(read_n(9999, 10000, 1 << 20, &got, true, -1) == PASTE_OK && got == 9999);
	CHECK(read_n(0, 10000, 1 << 20, &got, true, -1) == PASTE_OK && got == 0);
	/* a source that has not ended: more to come */
	CHECK(read_n(5000, 10000, 1 << 20, &got, false, -1) == PASTE_MORE && got == 5000);
	/* a turn's worth, then the rest the next time */
	CHECK(read_n(20000, 100000, 8192, &got, true, -1) == PASTE_MORE && got >= 8192 && got < 20000);
	/* far more than max: stops one byte past it */
	CHECK(read_n(100000, 10000, 1 << 20, &got, true, -1) == PASTE_TOO_LARGE && got == 10001);
	/* out of memory growing the buffer */
	CHECK(read_n(100, 10000, 1 << 20, &got, true, 0) == PASTE_NOMEM);
}

int main(void)
{
	test_utf8_seq();
	test_put_utf8();
	test_decode();
	test_filter();
	test_brackets();
	test_latin1();
	test_mime();
	test_read();
	printf("test_paste: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
