/* util.c - see util.h */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

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
