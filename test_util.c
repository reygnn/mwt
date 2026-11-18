/* test_util.c - the -g argument and the context menu's keys: make check */
#include <limits.h>
#include <stdio.h>

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

int main(void)
{
	test_geometry();
	test_menu();
	printf("test_util: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
