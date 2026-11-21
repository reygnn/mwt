/* test_box.c - box drawing and block elements at many cell sizes: lines that
 * reach the edges in the middle, at their weight, so that neighbours join;
 * double lines that meet as they should; blocks that tile. No font: make check */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "box.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

enum { MAXW = 64, MAXH = 128 };
static uint8_t a[MAXW * MAXH];

static const int sizes[][2] = {
	{ 5, 9 }, { 6, 12 }, { 7, 15 }, { 8, 16 }, { 9, 17 }, { 10, 21 }, { 11, 22 }, { 16, 32 }, { 18, 35 },
};

/* the weights of up, right, down and left, from the Unicode names: 0 none,
 * 1 light, 2 heavy, 3 double */
static const struct { unsigned cp; const char *arms; } lines[] = {
	{ 0x2500, "0101" }, { 0x2501, "0202" }, { 0x2502, "1010" }, { 0x2503, "2020" },
	{ 0x250c, "0110" }, { 0x250d, "0210" }, { 0x250e, "0120" }, { 0x250f, "0220" },
	{ 0x2510, "0011" }, { 0x2511, "0012" }, { 0x2512, "0021" }, { 0x2513, "0022" },
	{ 0x2514, "1100" }, { 0x2515, "1200" }, { 0x2516, "2100" }, { 0x2517, "2200" },
	{ 0x2518, "1001" }, { 0x2519, "1002" }, { 0x251a, "2001" }, { 0x251b, "2002" },
	{ 0x251c, "1110" }, { 0x251d, "1210" }, { 0x251e, "2110" }, { 0x251f, "1120" },
	{ 0x2520, "2120" }, { 0x2521, "2210" }, { 0x2522, "1220" }, { 0x2523, "2220" },
	{ 0x2524, "1011" }, { 0x2525, "1012" }, { 0x2526, "2011" }, { 0x2527, "1021" },
	{ 0x2528, "2021" }, { 0x2529, "2012" }, { 0x252a, "1022" }, { 0x252b, "2022" },
	{ 0x252c, "0111" }, { 0x252d, "0112" }, { 0x252e, "0211" }, { 0x252f, "0212" },
	{ 0x2530, "0121" }, { 0x2531, "0122" }, { 0x2532, "0221" }, { 0x2533, "0222" },
	{ 0x2534, "1101" }, { 0x2535, "1102" }, { 0x2536, "1201" }, { 0x2537, "1202" },
	{ 0x2538, "2101" }, { 0x2539, "2102" }, { 0x253a, "2201" }, { 0x253b, "2202" },
	{ 0x253c, "1111" }, { 0x253d, "1112" }, { 0x253e, "1211" }, { 0x253f, "1212" },
	{ 0x2540, "2111" }, { 0x2541, "1121" }, { 0x2542, "2121" }, { 0x2543, "2112" },
	{ 0x2544, "2211" }, { 0x2545, "1122" }, { 0x2546, "1221" }, { 0x2547, "2212" },
	{ 0x2548, "1222" }, { 0x2549, "2122" }, { 0x254a, "2221" }, { 0x254b, "2222" },
	{ 0x2550, "0303" }, { 0x2551, "3030" }, { 0x2552, "0310" }, { 0x2553, "0130" },
	{ 0x2554, "0330" }, { 0x2555, "0013" }, { 0x2556, "0031" }, { 0x2557, "0033" },
	{ 0x2558, "1300" }, { 0x2559, "3100" }, { 0x255a, "3300" }, { 0x255b, "1003" },
	{ 0x255c, "3001" }, { 0x255d, "3003" }, { 0x255e, "1310" }, { 0x255f, "3130" },
	{ 0x2560, "3330" }, { 0x2561, "1013" }, { 0x2562, "3031" }, { 0x2563, "3033" },
	{ 0x2564, "0313" }, { 0x2565, "0131" }, { 0x2566, "0333" }, { 0x2567, "1303" },
	{ 0x2568, "3101" }, { 0x2569, "3303" }, { 0x256a, "1313" }, { 0x256b, "3131" },
	{ 0x256c, "3333" }, { 0x2574, "0001" }, { 0x2575, "1000" }, { 0x2576, "0100" },
	{ 0x2577, "0010" }, { 0x2578, "0002" }, { 0x2579, "2000" }, { 0x257a, "0200" },
	{ 0x257b, "0020" }, { 0x257c, "0201" }, { 0x257d, "1020" }, { 0x257e, "0102" },
	{ 0x257f, "2010" },
};

/* Which band the ink across an edge of n pixels is: 0 none, 1 light (t),
 * 2 heavy (2t), 3 double (two of t, t apart), each in the middle; -1 for
 * anything else. */
static int band(const uint8_t *px, int n, int stride, int t)
{
	bool ink[MAXH], any = false;
	for (int i = 0; i < n; i++)
		any |= ink[i] = px[i * stride] >= 128;
	if (!any)
		return 0;
	for (int w = 1; w <= 3; w++) {
		bool want[MAXH] = { false };
		if (w < 3) {
			int tt = w * t, s = (n - tt) / 2;
			for (int i = s; i < s + tt; i++)
				want[i] = true;
		} else {
			int s = (n - 3 * t) / 2;
			for (int i = 0; i < t; i++)
				want[s + i] = want[s + 2 * t + i] = true;
		}
		if (!memcmp(ink, want, (size_t)n * sizeof *ink))
			return w;
	}
	return -1;
}

/* The ink of a line without double arms is one piece: arms that meet leave no
 * gap at the crossing, however their weights differ. */
static bool connected(int w, int h)
{
	static int stack[MAXW * MAXH];
	static bool seen[MAXW * MAXH];
	memset(seen, 0, sizeof seen);
	int start = -1, total = 0, n = 0, reached = 0;
	for (int i = 0; i < w * h; i++)
		if (a[i] == 255) {
			total++;
			if (start < 0)
				start = i;
		}
	if (start < 0)
		return false;
	stack[n++] = start;
	seen[start] = true;
	while (n) {
		int i = stack[--n], x = i % w, y = i / w;
		reached++;
		const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
		for (int k = 0; k < 4; k++) {
			int j = ny[k] * w + nx[k];
			if (nx[k] >= 0 && nx[k] < w && ny[k] >= 0 && ny[k] < h && !seen[j] && a[j] == 255) {
				seen[j] = true;
				stack[n++] = j;
			}
		}
	}
	return reached == total;
}

/* Every line character at every size, light strokes of 1 to 3 pixels: its
 * four edges carry the bands its name says, so that any two that meet join */
static void test_edges(void)
{
	int bad = 0;
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
		for (int t = 1; t <= 3; t++)
			for (int bold = 0; bold < 2; bold++) {
				int w = sizes[s][0], h = sizes[s][1];
				/* room for a double band with an empty edge beside it */
				if (3 * t + 2 > w)
					continue;
				for (size_t i = 0; i < sizeof lines / sizeof *lines; i++) {
					box_draw(lines[i].cp, w, h, t, bold, a);
					int e[4];
					e[0] = band(a, w, 1, t);                   /* top row */
					e[1] = band(a + w - 1, h, w, t);           /* right column */
					e[2] = band(a + (h - 1) * w, w, 1, t);     /* bottom row */
					e[3] = band(a, h, w, t);                   /* left column */
					if (!strchr(lines[i].arms, '3') && !connected(w, h)) {
						if (bad++ < 10)
							fprintf(stderr, "  U+%04X %dx%d t %d bold %d: "
								"in pieces\n", lines[i].cp, w, h, t, bold);
					}
					for (int k = 0; k < 4; k++) {
						int want = lines[i].arms[k] - '0';
						if (bold && want == 1)
							want = 2;
						if (e[k] != want) {
							if (bad++ < 10)
								fprintf(stderr, "  U+%04X %dx%d t %d bold %d: "
									"edge %d is %d, not %d\n", lines[i].cp,
									w, h, t, bold, k, e[k], want);
						}
					}
				}
			}
	CHECK(bad == 0);
}

/* Where double lines meet, at 9x11 with strokes of 1: the outer and inner lines
 * of corners, a tee's stroke broken where the arm comes in, a single line
 * through a double one or ending at its near stroke. */
static const struct { unsigned cp; const char *rows[11]; } pictures[] = {
	{ 0x2554, {
		"_________",
		"_________",
		"_________",
		"_________",
		"___######",
		"___#_____",
		"___#_####",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x256c, {
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"####_####",
		"_________",
		"####_####",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x2560, {
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_####",
		"___#_____",
		"___#_####",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x2566, {
		"_________",
		"_________",
		"_________",
		"_________",
		"#########",
		"_________",
		"####_####",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x255f, {
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_####",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x2564, {
		"_________",
		"_________",
		"_________",
		"_________",
		"#########",
		"_________",
		"#########",
		"____#____",
		"____#____",
		"____#____",
		"____#____",
	} },
	{ 0x2552, {
		"_________",
		"_________",
		"_________",
		"_________",
		"____#####",
		"____#____",
		"____#####",
		"____#____",
		"____#____",
		"____#____",
		"____#____",
	} },
	{ 0x256b, {
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"#########",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x2562, {
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"####_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
	{ 0x2556, {
		"_________",
		"_________",
		"_________",
		"_________",
		"_________",
		"######___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
		"___#_#___",
	} },
};

static void test_pictures(void)
{
	for (size_t i = 0; i < sizeof pictures / sizeof *pictures; i++) {
		box_draw(pictures[i].cp, 9, 11, 1, false, a);
		bool same = true;
		for (int y = 0; y < 11; y++)
			for (int x = 0; x < 9; x++)
				same &= (a[y * 9 + x] == 255) == (pictures[i].rows[y][x] == '#');
		CHECK(same);
		if (!same)
			fprintf(stderr, "  U+%04X differs\n", pictures[i].cp);
	}
}

static int ink_rows(int w, int h, int col)
{
	int n = 0;
	for (int y = 0; y < h; y++)
		n += a[y * w + col] == 255;
	return n;
}

/* heavy is twice light, bold makes light heavy and leaves double as it is */
static void test_weights(void)
{
	static uint8_t b[MAXW * MAXH];
	box_draw(0x2500, 9, 17, 2, false, a);
	CHECK(ink_rows(9, 17, 0) == 2 && a[7 * 9] == 255 && a[8 * 9] == 255);
	box_draw(0x2501, 9, 17, 2, false, b);
	CHECK(ink_rows(9, 17, 0) == 2);
	memcpy(a, b, sizeof a);
	CHECK(ink_rows(9, 17, 0) == 4);
	box_draw(0x2500, 9, 17, 2, true, a); /* bold ─ is ━ */
	CHECK(!memcmp(a, b, 9 * 17));
	box_draw(0x2550, 9, 17, 2, false, b);
	box_draw(0x2550, 9, 17, 2, true, a); /* bold ═ is ═ */
	CHECK(!memcmp(a, b, 9 * 17) && ink_rows(9, 17, 0) == 4);
	/* a stroke never wider than the cell, nor its double band */
	box_draw(0x2551, 5, 9, 4, true, a);
	CHECK(a[0] == 0 && a[1] == 255 && a[2] == 0 && a[3] == 255 && a[4] == 0);
	box_draw(0x2502, 3, 9, 8, false, a);
	CHECK(a[0] == 0 && a[1] == 255 && a[2] == 0);
}

static bool all(int w, int h, uint8_t v)
{
	for (int i = 0; i < w * h; i++)
		if (a[i] != v)
			return false;
	return true;
}

/* each pixel 255 in exactly one of the given, none left over */
static bool tiles(int w, int h, const unsigned *cps, int n)
{
	static uint8_t sum[MAXW * MAXH];
	memset(sum, 0, sizeof sum);
	for (int k = 0; k < n; k++) {
		box_draw(cps[k], w, h, 1, false, a);
		for (int i = 0; i < w * h; i++)
			sum[i] += a[i] == 255 ? 1 : a[i] ? 100 : 0;
	}
	for (int i = 0; i < w * h; i++)
		if (sum[i] != 1)
			return false;
	return true;
}

static void test_blocks(void)
{
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
		int w = sizes[s][0], h = sizes[s][1];
		box_draw(0x2588, w, h, 1, false, a);
		CHECK(all(w, h, 255));
		static const unsigned halves[] = { 0x2580, 0x2584 }, sides[] = { 0x258c, 0x2590 };
		static const unsigned tops[] = { 0x2594, 0x2587 }, rights[] = { 0x2589, 0x2595 };
		static const unsigned quads[] = { 0x2598, 0x259d, 0x2596, 0x2597 };
		static const unsigned q3[] = { 0x2599, 0x259d }, q2[] = { 0x259a, 0x259e };
		CHECK(tiles(w, h, halves, 2) && tiles(w, h, sides, 2));
		CHECK(tiles(w, h, tops, 2) && tiles(w, h, rights, 2));
		CHECK(tiles(w, h, quads, 4) && tiles(w, h, q3, 2) && tiles(w, h, q2, 2));
		static const unsigned q31[] = { 0x259b, 0x2597 }, q32[] = { 0x259c, 0x2596 };
		static const unsigned q33[] = { 0x259f, 0x2598 };
		CHECK(tiles(w, h, q31, 2) && tiles(w, h, q32, 2) && tiles(w, h, q33, 2));
		/* the eighths grow by rows from the bottom, by columns from the left */
		int last = 0;
		bool grows = true;
		for (unsigned cp = 0x2581; cp <= 0x2588; cp++) {
			box_draw(cp, w, h, 1, false, a);
			int n = ink_rows(w, h, 0);
			grows &= n >= last && a[(h - 1) * w] == 255;
			last = n;
		}
		CHECK(grows && last == h);
		box_draw(0x2584, w, h, 1, false, a); /* ▄ is four eighths */
		CHECK(ink_rows(w, h, 0) == h - (h * 4 + 4) / 8);
		/* the shades even, at a quarter, a half and three quarters */
		box_draw(0x2591, w, h, 1, false, a);
		CHECK(all(w, h, 64));
		box_draw(0x2592, w, h, 1, false, a);
		CHECK(all(w, h, 128));
		box_draw(0x2593, w, h, 1, false, a);
		CHECK(all(w, h, 191));
	}
}

/* runs of ink along the middle row (or column), and how thick the first is */
static int runs(int w, int h, bool down, int t, int *thick)
{
	int n = 0, len = down ? h : w, first = -1;
	bool in = false;
	for (int i = 0; i < len; i++) {
		bool ink = a[down ? i * w + (w - t) / 2 : ((h - t) / 2) * w + i] == 255;
		if (ink && first < 0)
			first = i;
		n += ink && !in;
		in = ink;
	}
	*thick = 0;
	for (int k = 0; first >= 0 && k < (down ? w : h); k++)
		*thick += a[down ? first * w + k : k * w + first] == 255;
	return n;
}

static void test_dashes(void)
{
	static const struct { unsigned cp; int n; bool down, heavy; } d[] = {
		{ 0x2504, 3, false, false }, { 0x2505, 3, false, true },
		{ 0x2506, 3, true, false }, { 0x2507, 3, true, true },
		{ 0x2508, 4, false, false }, { 0x2509, 4, false, true },
		{ 0x250a, 4, true, false }, { 0x250b, 4, true, true },
		{ 0x254c, 2, false, false }, { 0x254d, 2, false, true },
		{ 0x254e, 2, true, false }, { 0x254f, 2, true, true },
	};
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
		int w = sizes[s][0], h = sizes[s][1];
		for (size_t i = 0; i < sizeof d / sizeof *d; i++) {
			if (!d[i].down && w < 2 * d[i].n) {
				/* no room for a gap after each: still no dash lost */
				box_draw(d[i].cp, w, h, 1, false, a);
				int ink = 0, t = d[i].heavy ? 2 : 1;
				for (int x = 0; x < w; x++)
					ink += a[((h - t) / 2) * w + x] == 255;
				CHECK(ink >= d[i].n);
				continue;
			}
			int t = d[i].heavy ? 2 : 1, thick;
			box_draw(d[i].cp, w, h, 1, false, a);
			int n = runs(w, h, d[i].down, t, &thick);
			CHECK(n == d[i].n && thick == t);
			if (n != d[i].n || thick != t)
				fprintf(stderr, "  U+%04X at %dx%d: %d dashes, %d thick\n", d[i].cp,
					w, h, n, thick);
		}
	}
}

/* An arc leaves the cell where the straight lines do, at the middle of two
 * edges, and touches no other; a diagonal runs corner to corner. */
static void test_arcs(void)
{
	static const struct { unsigned cp; int right, down; } arc[] = {
		{ 0x256d, 1, 1 }, { 0x256e, 0, 1 }, { 0x256f, 0, 0 }, { 0x2570, 1, 0 },
	};
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
		int w = sizes[s][0], h = sizes[s][1];
		for (size_t i = 0; i < 4; i++) {
			box_draw(arc[i].cp, w, h, 1, false, a);
			int row = arc[i].down ? h - 1 : 0, col = arc[i].right ? w - 1 : 0;
			int orow = h - 1 - row, ocol = w - 1 - col;
			bool ok = a[row * w + (w - 1) / 2] == 255 && a[((h - 1) / 2) * w + col] >= 128;
			for (int x = 0; x < w; x++)
				ok &= a[orow * w + x] == 0;
			for (int y = 0; y < h; y++)
				ok &= a[y * w + ocol] == 0;
			/* as round as the cell allows: the quarter circle as large as
			 * fits between the middle lines and every edge, the same for
			 * all four, ink where it crosses the diagonal */
			double cx = (w - 1) / 2 + 0.5, cy = (h - 1) / 2 + 0.5;
			double r = w - cx < cx ? w - cx : cx, ry = h - cy < cy ? h - cy : cy;
			r = r < ry ? r : ry;
			double k = r * (1 - 0.70710678);
			int px = (int)(cx + (arc[i].right ? k : -k)), py = (int)(cy + (arc[i].down ? k : -k));
			ok &= a[py * w + px] >= 64;
			CHECK(ok);
			if (!ok)
				fprintf(stderr, "  U+%04X at %dx%d\n", arc[i].cp, w, h);
		}
		box_draw(0x2571, w, h, 1, false, a);
		CHECK(a[w - 1] >= 128 && a[(h - 1) * w] >= 128 && a[0] == 0 && a[h * w - 1] == 0);
		box_draw(0x2572, w, h, 1, false, a);
		CHECK(a[0] >= 128 && a[h * w - 1] >= 128 && a[w - 1] == 0 && a[(h - 1) * w] == 0);
		box_draw(0x2573, w, h, 1, false, a);
		CHECK(a[0] >= 128 && a[h * w - 1] >= 128 && a[w - 1] >= 128 && a[(h - 1) * w] >= 128);
	}
}

/* the range, and every character at cells down to one pixel and strokes wider
 * than the cell, under the sanitizers */
static void test_range(void)
{
	CHECK(!box_covers(0x24ff) && box_covers(0x2500) && box_covers(0x259f) && !box_covers(0x25a0));
	memset(a, 7, sizeof a);
	box_draw(0x25a0, 4, 4, 1, false, a);
	CHECK(all(4, 4, 0));
	int empty = 0;
	for (int w = 1; w <= 12; w++)
		for (int h = 1; h <= 24; h++)
			for (int t = 1; t <= 8; t += 3)
				for (unsigned cp = 0x23ba; cp <= 0x259f; cp = cp == 0x23bd ? 0x2500 : cp + 1) {
					box_draw(cp, w, h, t, cp & 1, a);
					bool ink = false;
					for (int i = 0; i < w * h; i++)
						ink |= a[i] != 0;
					empty += !ink && w >= 8 && h >= 8;
				}
	CHECK(empty == 0);
	box_draw(0x2500, 0, 5, 1, false, a); /* nothing to draw into */
	box_draw(0x2500, MAXW, MAXH, 1, false, a);
	CHECK(a[((MAXH - 1) / 2) * MAXW] == 255 && a[((MAXH - 1) / 2) * MAXW + MAXW - 1] == 255);
}

/* The DEC scan lines 1, 3, 7 and 9 of nine, one light stroke across the cell
 * each, top to bottom; 5 would be ─. The blocks are what keeps its colors. */
static void test_scan(void)
{
	for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
		for (int t = 1; t <= 2; t++)
			for (int bold = 0; bold < 2; bold++) {
				int w = sizes[s][0], h = sizes[s][1], tt = bold ? 2 * t : t;
				int at[4], last = -1;
				bool ok = true;
				for (unsigned cp = 0x23ba; cp <= 0x23bd; cp++) {
					box_draw(cp, w, h, t, bold, a);
					int first = -1, rows = 0;
					for (int y = 0; y < h; y++)
						if (a[y * w] == 255 && a[y * w + w - 1] == 255) {
							first = first < 0 ? y : first;
							rows++;
						}
					at[cp - 0x23ba] = first;
					ok &= rows == tt && first > last && ink_rows(w, h, w / 2) == tt;
					last = first;
				}
				ok &= at[0] == 0 && at[3] == h - tt;
				ok &= at[1] < (h - tt) / 2 && at[2] > (h - tt) / 2;
				CHECK(ok);
				if (!ok)
					fprintf(stderr, "  scan lines at %dx%d t %d bold %d: %d %d %d %d\n", w,
						h, t, bold, at[0], at[1], at[2], at[3]);
			}
	CHECK(box_covers(0x23ba) && box_covers(0x23bd) && !box_covers(0x23b9) && !box_covers(0x23be));
	CHECK(box_block(0x2580) && box_block(0x2591) && box_block(0x259f));
	CHECK(!box_block(0x257f) && !box_block(0x25a0) && !box_block(0x23ba));
}

/* the light stroke: the underline's, rounded, a tenth of the cell without one,
 * never thinner than a pixel a scale */
static void test_thin(void)
{
	CHECK(box_thin(0.31, 9, 1) == 1 && box_thin(1.6, 9, 1) == 2 && box_thin(2.4, 9, 1) == 2);
	CHECK(box_thin(0, 20, 1) == 2 && box_thin(-1, 30, 1) == 3 && box_thin(0, 4, 1) == 1);
	CHECK(box_thin(0.5, 9, 2) == 2 && box_thin(5, 9, 3) == 5 && box_thin(0, 9, 3) == 3);
	CHECK(box_thin(0.2, 9, 0) == 1);
}

int main(void)
{
	test_thin();
	test_scan();
	test_range();
	test_edges();
	test_pictures();
	test_weights();
	test_blocks();
	test_dashes();
	test_arcs();
	printf("test_box: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
