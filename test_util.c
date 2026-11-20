/* test_util.c - the -g argument, the context menu's keys and the standard
 * fds: make check */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "util.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

static int geom(const char *s, int *c, int *r)
{
	*c = *r = -7;
	return parse_geometry(s, 1000, 500, c, r);
}

static void test_geometry(void)
{
	int c, r;
	CHECK(geom("80x24", &c, &r) == GEOM_OK && c == 80 && r == 24);
	CHECK(geom("1x1", &c, &r) == GEOM_OK && c == 1 && r == 1);
	CHECK(geom("1000x500", &c, &r) == GEOM_OK && c == 1000 && r == 500);
	/* as strtol reads it */
	CHECK(geom("+80x+24", &c, &r) == GEOM_OK && c == 80 && r == 24);
	CHECK(geom(" 80x 24", &c, &r) == GEOM_OK && c == 80 && r == 24);
	CHECK(geom("080x024", &c, &r) == GEOM_OK && c == 80 && r == 24);
	/* out of range: nothing set */
	CHECK(geom("0x24", &c, &r) == GEOM_RANGE && c == -7 && r == -7);
	CHECK(geom("80x0", &c, &r) == GEOM_RANGE && c == -7);
	CHECK(geom("-80x24", &c, &r) == GEOM_RANGE);
	CHECK(geom("1001x24", &c, &r) == GEOM_RANGE);
	CHECK(geom("80x501", &c, &r) == GEOM_RANGE);
	CHECK(geom("99999999999999999999x1", &c, &r) == GEOM_SYNTAX); /* ERANGE */
	CHECK(geom("99999999999x1", &c, &r) == GEOM_RANGE);
	CHECK(geom("4294967377x24", &c, &r) == GEOM_RANGE); /* not 81 as an int */
	/* not COLSxROWS */
	static const char *bad[] = {
		"", "x", "80", "80x", "x24", "80X24", "80*24", "80x24 ", "80x24x1", "80 x24",
		"0x80x24", "abc", "80xabc", "80.5x24", "8e1x24", "80x24\n",
	};
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		checks++;
		if (geom(bad[i], &c, &r) != GEOM_SYNTAX || c != -7 || r != -7) {
			failures++;
			fprintf(stderr, "FAIL: \"%s\" taken as geometry\n", bad[i]);
		}
	}
}

static void test_menu(void)
{
	enum { ALL = 7 };
	/* Down from none is the first, Up from none the last */
	CHECK(menu_next(-1, 3, 1, ALL) == 0);
	CHECK(menu_next(-1, 3, -1, ALL) == 2);
	/* on and round the end */
	CHECK(menu_next(0, 3, 1, ALL) == 1);
	CHECK(menu_next(2, 3, 1, ALL) == 0);
	CHECK(menu_next(0, 3, -1, ALL) == 2);
	CHECK(menu_next(1, 3, -1, ALL) == 0);
	/* past what cannot be chosen: Copy without a selection */
	CHECK(menu_next(-1, 3, 1, 6) == 1);
	CHECK(menu_next(2, 3, 1, 6) == 1);
	CHECK(menu_next(1, 3, -1, 6) == 2);
	CHECK(menu_next(-1, 3, -1, 2) == 1);
	CHECK(menu_next(1, 3, 1, 2) == 1);
	/* none can be chosen: where it was */
	CHECK(menu_next(-1, 3, 1, 0) == -1);
	CHECK(menu_next(2, 3, -1, 0) == 2);
	/* two items: Up from none is the last as well */
	CHECK(menu_next(-1, 2, -1, 3) == 1);
	CHECK(menu_next(-1, 2, 1, 3) == 0);
	/* one item */
	CHECK(menu_next(-1, 1, 1, 1) == 0);
	CHECK(menu_next(-1, 1, -1, 1) == 0);
	CHECK(menu_next(0, 1, -1, 1) == 0);
	/* every start, both ways, any items: the next enabled one round the end */
	long bad = 0;
	for (int n = 1; n <= 8; n++)
		for (unsigned on = 0; on < 1u << n; on++)
			for (int h = -1; h < n; h++)
				for (int dir = -1; dir <= 1; dir += 2) {
					int want = h, from = h < 0 ? (dir > 0 ? -1 : n) : h;
					for (int k = 1; k <= n; k++) {
						int i = ((from + dir * k) % n + n) % n;
						if (on & 1u << i) {
							want = i;
							break;
						}
					}
					bad += menu_next(h, n, dir, on) != want;
				}
	CHECK(bad == 0);
}

/* whether fd is open on /dev/null */
static bool devnull(int fd)
{
	struct stat a, b;
	return fstat(fd, &a) == 0 && stat("/dev/null", &b) == 0 && a.st_rdev == b.st_rdev &&
	       S_ISCHR(a.st_mode);
}

static void test_clip(void)
{
	CHECK(utf8_clip("", 4) == 0);
	CHECK(utf8_clip("abc", 4) == 3);
	CHECK(utf8_clip("abcd", 4) == 4);
	CHECK(utf8_clip("abcde", 4) == 4);
	/* ä is two bytes: not cut in half */
	CHECK(utf8_clip("ab\xc3\xa4", 3) == 2);
	CHECK(utf8_clip("ab\xc3\xa4", 4) == 4);
	/* a four byte emoji at the end, cut anywhere inside */
	CHECK(utf8_clip("a\xf0\x9f\x99\x82", 2) == 1);
	CHECK(utf8_clip("a\xf0\x9f\x99\x82", 4) == 1);
	CHECK(utf8_clip("a\xf0\x9f\x99\x82", 5) == 5);
	CHECK(utf8_clip("a\xf0\x9f\x99\x82" "b", 5) == 5);
	/* invalid bytes are no sequence to keep whole: cut where it falls */
	CHECK(utf8_clip("\xff\xff\xff", 2) == 2);
	/* only continuation bytes: nothing left */
	CHECK(utf8_clip("\x80\x80\x80", 2) == 0);
	/* a long one reads no further than max + 1 */
	static char big[70000];
	memset(big, 'x', sizeof big - 1);
	CHECK(utf8_clip(big, 1023) == 1023);
}

static void test_snap(void)
{
	int w, h;
	/* 80x24 cells of 9x18, padding 2: 724x436 */
	snap_size(80, 24, 9, 18, 2, 1, &w, &h);
	CHECK(w == 724 && h == 436);
	/* scale 2, cells of 18x35, padding 4: 1448x848 buffer pixels, half of them
	 * logical, rounded up so the buffer holds the grid */
	snap_size(80, 24, 18, 35, 4, 2, &w, &h);
	CHECK(w == 724 && h == 424);
	snap_size(1, 1, 17, 35, 4, 2, &w, &h);
	CHECK(w == 13 && h == 22 && w * 2 >= 17 + 8 && h * 2 >= 35 + 8);
	/* the largest grid at a large cell: no overflow on the way */
	snap_size(4096, 2048, 100000, 100000, 4, 4, &w, &h);
	CHECK(w == (int)((4096LL * 100000 + 8 + 3) / 4));
}

static void test_bounds(void)
{
	int c = 400, r = 200;
	/* 1000x700 at scale 1, padding 2, cells 9x18: 110 by 38 */
	fit_bounds(&c, &r, 1000, 700, 1, 2, 9, 18);
	CHECK(c == 110 && r == 38);
	/* what fits stays */
	c = 80, r = 24;
	fit_bounds(&c, &r, 1000, 700, 1, 2, 9, 18);
	CHECK(c == 80 && r == 24);
	/* no bounds */
	c = 400, r = 200;
	fit_bounds(&c, &r, 0, 0, 1, 2, 9, 18);
	CHECK(c == 400 && r == 200);
	/* scale 2: buffer pixels, padding 4, cells 18x36 */
	fit_bounds(&c, &r, 1000, 700, 2, 4, 18, 36);
	CHECK(c == 110 && r == 38);
	/* only one side bounded */
	c = 400, r = 200;
	fit_bounds(&c, &r, 1000, 0, 1, 2, 9, 18);
	CHECK(c == 110 && r == 200);
	/* bounds a compositor should not send: no overflow */
	c = 80, r = 24;
	fit_bounds(&c, &r, 2147483647, 2147483647, 4, 2, 1, 1);
	CHECK(c == 80 && r == 24);
	/* smaller than a cell: one */
	c = 80, r = 24;
	fit_bounds(&c, &r, 3, 3, 1, 2, 9, 18);
	CHECK(c == 1 && r == 1);
}

/* in a child, with some of 0 to 2 closed: those come back on /dev/null, the
 * rest stays, and what is opened next is 3 */
static void test_stdio(void)
{
	static const int closed[] = { 1, 2, 4, 5, 7 }; /* bit i: fd i is closed */
	for (size_t k = 0; k < sizeof closed / sizeof closed[0]; k++) {
		fflush(stdout);
		fflush(stderr);
		pid_t pid = fork();
		if (pid == 0) {
			int keep = dup(2), p[2]; /* stderr kept, for what fails */
			struct stat was[3];
			for (int fd = 0; fd < 3; fd++)
				fstat(fd, &was[fd]);
			for (int fd = 0; fd < 3; fd++)
				if (closed[k] >> fd & 1)
					close(fd);
			int bad = !stdio_fill();
			for (int fd = 0; fd < 3; fd++) {
				struct stat now;
				char c;
				if (closed[k] >> fd & 1)
					bad += !devnull(fd) || write(fd, "x", 1) != 1 || read(fd, &c, 1) != 0;
				else
					bad += fstat(fd, &now) != 0 || now.st_ino != was[fd].st_ino ||
					       now.st_dev != was[fd].st_dev;
			}
			bad += pipe(p) != 0 || p[0] < 3 || p[1] < 3;
			if (bad)
				dprintf(keep, "test_util: stdio with mask %d: %d wrong\n", closed[k], bad);
			_exit(bad != 0);
		}
		int st = 0;
		CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
	}
	CHECK(stdio_fill()); /* nothing closed: nothing to do */
}

int main(void)
{
	test_geometry();
	test_menu();
	test_clip();
	test_bounds();
	test_snap();
	test_stdio();
	printf("test_util: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
