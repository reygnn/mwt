/* util.c - see util.h */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <string.h>

#include "util.h"

int parse_geometry(const char *s, int max_cols, int max_rows, int *cols, int *rows)
{
	char *end;
	errno = 0;
	long c = strtol(s, &end, 10);
	if (errno || end == s || *end != 'x')
		return GEOM_SYNTAX;
	const char *r0 = end + 1;
	long r = strtol(r0, &end, 10);
	if (errno || end == r0 || *end)
		return GEOM_SYNTAX;
	if (c < 1 || r < 1 || c > max_cols || r > max_rows)
		return GEOM_RANGE;
	*cols = (int)c;
	*rows = (int)r;
	return GEOM_OK;
}

int menu_next(int hover, int n, int dir, unsigned enabled)
{
	/* by dir, not by step: with two items Up is a step of 1 too */
	int step = dir > 0 ? 1 : n - 1;
	int i = hover >= 0 ? hover : dir > 0 ? n - 1 : 0;
	for (int k = 0; k < n; k++) {
		i = (i + step) % n;
		if (enabled & 1u << i)
			return i;
	}
	return hover;
}

bool stdio_fill(void)
{
	for (int fd = 0; fd < 3; fd++) {
		if (fcntl(fd, F_GETFD) >= 0 || errno != EBADF)
			continue;
		int n = open("/dev/null", O_RDWR); /* fd itself: those below are open */
		if (n < 0)
			return false;
		if (n != fd) {
			dup2(n, fd);
			close(n);
		}
	}
	return true;
}

static const char *token_usable(const char *s)
{
	return s && *s && strnlen(s, TOKEN_MAX + 1) <= TOKEN_MAX ? s : NULL;
}

char *startup_token(void)
{
	const char *s = token_usable(getenv("XDG_ACTIVATION_TOKEN"));
	if (!s)
		s = token_usable(getenv("DESKTOP_STARTUP_ID"));
	char *tok = s ? strdup(s) : NULL;
	unsetenv("XDG_ACTIVATION_TOKEN");
	unsetenv("DESKTOP_STARTUP_ID");
	return tok;
}

void toast_text(char *buf, size_t size, const char *name, int err)
{
	locale_t c = err == ENOENT ? (locale_t)0 : newlocale(LC_ALL_MASK, "C", (locale_t)0);
	snprintf(buf, size, "%s: %s", name, err == ENOENT ? "not found" :
		 c ? strerror_l(err, c) : strerror(err));
	if (c)
		freelocale(c);
}

int toast_fit(uint32_t *cp, int n, int fit)
{
	if (n <= fit)
		return n;
	if (fit < 1)
		return 0;
	memmove(cp + 1, cp + n - (fit - 1), (size_t)(fit - 1) * sizeof *cp);
	cp[0] = 0x2026;
	return fit;
}

/* One code point from p, U+FFFD for a byte that starts none; its length. */
static int utf8_one(const unsigned char *p, uint32_t *cp)
{
	int k = *p < 0x80 ? 1 : *p >= 0xc2 && *p < 0xe0 ? 2 :
		*p >= 0xe0 && *p < 0xf0 ? 3 : *p >= 0xf0 && *p < 0xf5 ? 4 : 0;
	uint32_t c = k == 1 ? *p : k ? *p & (0x7f >> k) : 0;
	for (int i = 1; i < k; i++) {
		if ((p[i] & 0xc0) != 0x80) {
			k = 0;
			break;
		}
		c = c << 6 | (p[i] & 0x3f);
	}
	/* overlong, surrogate or past U+10FFFF: not UTF-8 either */
	if (k == 3 && (c < 0x800 || (c >= 0xd800 && c < 0xe000)))
		k = 0;
	if (k == 4 && (c < 0x10000 || c > 0x10ffff))
		k = 0;
	*cp = k ? c : 0xfffd;
	return k ? k : 1;
}

int toast_cps(const char *s, uint32_t *cp, int max)
{
	const unsigned char *p = (const unsigned char *)s;
	uint32_t c;
	int total = 0;
	for (const unsigned char *q = p; *q; total++)
		q += utf8_one(q, &c);
	int n = 0;
	for (int i = 0; *p; i++) {
		p += utf8_one(p, &c);
		if (i >= total - max) /* the last max: the end says what went wrong */
			cp[n++] = c;
	}
	return n;
}

bool toast_place(int n, int win_w, int win_h, int cw, int ch, int scale,
		 struct toast_box *b)
{
	if (cw <= 0 || ch <= 0 || scale <= 0)
		return false;
	int room = (win_w - 2 * TOAST_MARGIN - 2 * TOAST_PAD) * scale / cw;
	if (n > room)
		n = room;
	int h = (ch + scale - 1) / scale + 2 * TOAST_PAD;
	if (n < 1 || TOAST_MARGIN + h > win_h)
		return false;
	b->n = n;
	b->w = (n * cw + scale - 1) / scale + 2 * TOAST_PAD;
	b->h = h;
	b->x = (win_w - b->w) / 2;
	b->y = TOAST_MARGIN;
	return true;
}

size_t utf8_clip(const char *s, size_t max)
{
	size_t n = strnlen(s, max + 1);
	if (n <= max)
		return n;
	n = max;
	/* back over continuation bytes to the start of the cut sequence */
	while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80)
		n--;
	return n;
}

void fit_bounds(int *cols, int *rows, int bw, int bh, int scale, int pad, int cw, int ch)
{
	if (bw > 0) {
		long long n = ((long long)bw * scale - 2 * pad) / cw;
		*cols = *cols < n ? *cols : n < 1 ? 1 : (int)n;
	}
	if (bh > 0) {
		long long n = ((long long)bh * scale - 2 * pad) / ch;
		*rows = *rows < n ? *rows : n < 1 ? 1 : (int)n;
	}
}

void snap_size(int cols, int rows, int cw, int ch, int pad, int scale, int *w, int *h)
{
	*w = (int)(((long long)cols * cw + 2 * pad + scale - 1) / scale);
	*h = (int)(((long long)rows * ch + 2 * pad + scale - 1) / scale);
}

bool shift_span(int pad, int ch, int top, int bot, int n, int h, int *y0, int *y1, int *px)
{
	*y0 = pad + top * ch;
	*y1 = pad + (bot + 1) * ch;
	*px = n * ch;
	return *y1 <= h;
}
