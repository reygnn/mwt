/* util.c - see util.h */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
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
		int n = ((long long)bw * scale - 2 * pad) / cw;
		*cols = *cols < n ? *cols : n < 1 ? 1 : n;
	}
	if (bh > 0) {
		int n = ((long long)bh * scale - 2 * pad) / ch;
		*rows = *rows < n ? *rows : n < 1 ? 1 : n;
	}
}
