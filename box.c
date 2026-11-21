/* box.c - see box.h */
#include <math.h>
#include <string.h>

#include "box.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

enum { NONE, LIGHT, HEAVY, DOUBLE };

/* U+2500 to U+257F: the weight of each arm, two bits each, up, right, down and
 * left from the low bits; 0 for the dashes, arcs and diagonals, drawn apart.
 * Made from the Unicode names. */
static const uint8_t arms[128] = {
	0x44, 0x88, 0x11, 0x22, 0x00, 0x00, 0x00, 0x00, /* 2500 */
	0x00, 0x00, 0x00, 0x00, 0x14, 0x18, 0x24, 0x28, /* 2508 */
	0x50, 0x90, 0x60, 0xa0, 0x05, 0x09, 0x06, 0x0a, /* 2510 */
	0x41, 0x81, 0x42, 0x82, 0x15, 0x19, 0x16, 0x25, /* 2518 */
	0x26, 0x1a, 0x29, 0x2a, 0x51, 0x91, 0x52, 0x61, /* 2520 */
	0x62, 0x92, 0xa1, 0xa2, 0x54, 0x94, 0x58, 0x98, /* 2528 */
	0x64, 0xa4, 0x68, 0xa8, 0x45, 0x85, 0x49, 0x89, /* 2530 */
	0x46, 0x86, 0x4a, 0x8a, 0x55, 0x95, 0x59, 0x99, /* 2538 */
	0x56, 0x65, 0x66, 0x96, 0x5a, 0xa5, 0x69, 0x9a, /* 2540 */
	0xa9, 0xa6, 0x6a, 0xaa, 0x00, 0x00, 0x00, 0x00, /* 2548 */
	0xcc, 0x33, 0x1c, 0x34, 0x3c, 0xd0, 0x70, 0xf0, /* 2550 */
	0x0d, 0x07, 0x0f, 0xc1, 0x43, 0xc3, 0x1d, 0x37, /* 2558 */
	0x3f, 0xd1, 0x73, 0xf3, 0xdc, 0x74, 0xfc, 0xcd, /* 2560 */
	0x47, 0xcf, 0xdd, 0x77, 0xff, 0x00, 0x00, 0x00, /* 2568 */
	0x00, 0x00, 0x00, 0x00, 0x40, 0x01, 0x04, 0x10, /* 2570 */
	0x80, 0x02, 0x08, 0x20, 0x48, 0x21, 0x84, 0x12, /* 2578 */
};

struct canvas {
	uint8_t *a;
	int w, h;
};

static void fill(const struct canvas *c, int x0, int y0, int x1, int y1, uint8_t v)
{
	x0 = MAX(x0, 0);
	y0 = MAX(y0, 0);
	x1 = MIN(x1, c->w);
	y1 = MIN(y1, c->h);
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			c->a[y * c->w + x] = v;
}

/* where a stroke t wide starts to sit in the middle of n: the same rule for
 * every weight, so a heavy stroke covers a light one */
static int mid(int n, int t)
{
	return (n - t) / 2;
}

struct lines {
	int t[4];  /* light and heavy thickness by weight; t[DOUBLE] one stroke */
	int u, r, d, l;
};

/* The horizontal arms, left (dir -1) or right (dir 1), of weight w, against
 * what runs vertically: v the heavier of up and down that is not double, its
 * band [v0, v1), and vb the start of a double band of three strokes. */
static void horizontal(const struct canvas *c, const struct lines *s, int dir)
{
	int w = dir < 0 ? s->l : s->r, other = dir < 0 ? s->r : s->l;
	if (!w)
		return;
	int td = s->t[DOUBLE];
	int vw = MAX(s->u == DOUBLE ? 0 : s->u, s->d == DOUBLE ? 0 : s->d);
	bool vdouble = s->u == DOUBLE || s->d == DOUBLE;
	bool vstraight = s->u == DOUBLE && s->d == DOUBLE;
	int v0 = mid(c->w, s->t[vw]), v1 = v0 + s->t[vw];
	int vb = mid(c->w, 3 * td);
	if (w != DOUBLE) {
		int t = s->t[w], y = mid(c->h, t);
		int x0, x1;
		/* through a straight double line only on to the other side; to a
		 * corner of one, its far stroke */
		if (dir < 0) {
			x0 = 0;
			x1 = vdouble ? (vstraight && !other ? vb + td : vb + 3 * td) :
			     vw ? v1 : mid(c->w, t) + t;
		} else {
			x1 = c->w;
			x0 = vdouble ? (vstraight && !other ? vb + 2 * td : vb) :
			     vw ? v0 : mid(c->w, t);
		}
		fill(c, x0, y, x1, y + t, 255);
		return;
	}
	int hb = mid(c->h, 3 * td);
	for (int k = 0; k < 2; k++) { /* the upper stroke faces up, the lower down */
		int y = hb + 2 * k * td;
		bool faced = k ? s->d == DOUBLE : s->u == DOUBLE;
		int x0, x1;
		/* a stroke facing a double arm turns into its near stroke, one
		 * facing none goes on to the far one: the outer and inner lines of
		 * a corner */
		if (dir < 0) {
			x0 = 0;
			x1 = vdouble ? (faced ? vb + td : vb + 3 * td) : vw ? v1 : c->w;
		} else {
			x1 = c->w;
			x0 = vdouble ? (faced ? vb + 2 * td : vb) : vw ? v0 : 0;
		}
		fill(c, x0, y, x1, y + td, 255);
	}
}

/* The same for up (dir -1) and down (dir 1), against what runs horizontally. */
static void vertical(const struct canvas *c, const struct lines *s, int dir)
{
	int w = dir < 0 ? s->u : s->d, other = dir < 0 ? s->d : s->u;
	if (!w)
		return;
	int td = s->t[DOUBLE];
	int hw = MAX(s->l == DOUBLE ? 0 : s->l, s->r == DOUBLE ? 0 : s->r);
	bool hdouble = s->l == DOUBLE || s->r == DOUBLE;
	bool hstraight = s->l == DOUBLE && s->r == DOUBLE;
	int h0 = mid(c->h, s->t[hw]), h1 = h0 + s->t[hw];
	int hb = mid(c->h, 3 * td);
	if (w != DOUBLE) {
		int t = s->t[w], x = mid(c->w, t);
		int y0, y1;
		if (dir < 0) {
			y0 = 0;
			y1 = hdouble ? (hstraight && !other ? hb + td : hb + 3 * td) :
			     hw ? h1 : mid(c->h, t) + t;
		} else {
			y1 = c->h;
			y0 = hdouble ? (hstraight && !other ? hb + 2 * td : hb) :
			     hw ? h0 : mid(c->h, t);
		}
		fill(c, x, y0, x + t, y1, 255);
		return;
	}
	int vb = mid(c->w, 3 * td);
	for (int k = 0; k < 2; k++) { /* the left stroke faces left */
		int x = vb + 2 * k * td;
		bool faced = k ? s->r == DOUBLE : s->l == DOUBLE;
		int y0, y1;
		if (dir < 0) {
			y0 = 0;
			y1 = hdouble ? (faced ? hb + td : hb + 3 * td) : hw ? h1 : c->h;
		} else {
			y1 = c->h;
			y0 = hdouble ? (faced ? hb + 2 * td : hb) : hw ? h0 : 0;
		}
		fill(c, x, y0, x + td, y1, 255);
	}
}

/* n dashes across (or down) the cell, a gap split over both ends of each, so
 * that a row of them keeps its rhythm from cell to cell */
static void dashes(const struct canvas *c, int n, int t, bool down)
{
	int len = down ? c->h : c->w;
	int gap = MAX(1, len / (4 * n));
	int p = mid(down ? c->w : c->h, t);
	for (int i = 0; i < n; i++) {
		int s0 = (i * len + n / 2) / n, s1 = ((i + 1) * len + n / 2) / n;
		int g = MAX(0, MIN(gap, s1 - s0 - 1)); /* a dash keeps a pixel */
		int a = s0 + g / 2, b = s1 - (g - g / 2);
		if (down)
			fill(c, p, a, p + t, b, 255);
		else
			fill(c, a, p, b, p + t, 255);
	}
}

static uint8_t cover(double d, int t)
{
	double v = t / 2.0 + 0.5 - d;
	return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)lround(v * 255);
}

/* distance from (px, py) to the segment (ax, ay) to (bx, by) */
static double seg(double px, double py, double ax, double ay, double bx, double by)
{
	double dx = bx - ax, dy = by - ay, l = dx * dx + dy * dy;
	double k = l > 0 ? ((px - ax) * dx + (py - ay) * dy) / l : 0;
	k = k < 0 ? 0 : k > 1 ? 1 : k;
	return hypot(px - ax - k * dx, py - ay - k * dy);
}

/* A rounded corner with its arms toward dx and dy (1 right or down, -1 left or
 * up): the centre lines of the straight ones, joined by a quarter circle as
 * large as the cell allows on every side, so it leaves the cell where they do. */
static void arc(const struct canvas *c, int t, int dx, int dy)
{
	double cx = mid(c->w, t) + t / 2.0, cy = mid(c->h, t) + t / 2.0;
	/* the same for all four corners: on an even width ╭ and ╮ differed */
	double r = MIN(MIN(c->w - cx, cx), MIN(c->h - cy, cy));
	double ox = cx + dx * r, oy = cy + dy * r; /* the circle's centre */
	double ex = dx > 0 ? c->w : 0, ey = dy > 0 ? c->h : 0;
	for (int y = 0; y < c->h; y++)
		for (int x = 0; x < c->w; x++) {
			double px = x + 0.5, py = y + 0.5, d;
			if ((px - ox) * dx <= 0 && (py - oy) * dy <= 0)
				d = fabs(hypot(px - ox, py - oy) - r);
			else
				d = MIN(seg(px, py, cx, oy, cx, ey), seg(px, py, ox, cy, ex, cy));
			c->a[y * c->w + x] = cover(d, t);
		}
}

/* ╱ (rise 1), ╲ (rise -1), from corner to corner */
static void diagonal(const struct canvas *c, int t, int rise)
{
	double w = c->w, h = c->h, l = hypot(w, h);
	for (int y = 0; y < c->h; y++)
		for (int x = 0; x < c->w; x++) {
			double px = x + 0.5, py = rise > 0 ? h - (y + 0.5) : y + 0.5;
			double d = fabs(px * h - py * w) / l;
			uint8_t v = cover(d, t);
			uint8_t *a = &c->a[y * c->w + x];
			*a = MAX(*a, v);
		}
}

/* the eighth k of n, rounded alike for every block, so that complements tile */
static int eighth(int n, int k)
{
	return (n * k + 4) / 8;
}

static void block(const struct canvas *c, uint32_t cp)
{
	int w = c->w, h = c->h, xm = eighth(w, 4), ym = eighth(h, 4);
	/* the quadrants, upper left, upper right, lower left, lower right */
	static const uint8_t quads[] = {
		[0x96 - 0x96] = 4, [0x97 - 0x96] = 8, [0x98 - 0x96] = 1,
		[0x99 - 0x96] = 1 | 4 | 8, [0x9a - 0x96] = 1 | 8, [0x9b - 0x96] = 1 | 2 | 4,
		[0x9c - 0x96] = 1 | 2 | 8, [0x9d - 0x96] = 2, [0x9e - 0x96] = 2 | 4,
		[0x9f - 0x96] = 2 | 4 | 8,
	};
	unsigned k = cp & 0xff;
	if (k == 0x80) {
		fill(c, 0, 0, w, ym, 255); /* upper half */
	} else if (k >= 0x81 && k <= 0x88) {
		fill(c, 0, eighth(h, 8 - (int)(k - 0x80)), w, h, 255); /* lower eighths */
	} else if (k >= 0x89 && k <= 0x8f) {
		fill(c, 0, 0, eighth(w, 8 - (int)(k - 0x88)), h, 255); /* left eighths */
	} else if (k == 0x90) {
		fill(c, xm, 0, w, h, 255); /* right half */
	} else if (k >= 0x91 && k <= 0x93) {
		/* shades: an even cover of a quarter, a half, three quarters */
		static const uint8_t shade[] = { 64, 128, 191 };
		fill(c, 0, 0, w, h, shade[k - 0x91]);
	} else if (k == 0x94) {
		fill(c, 0, 0, w, eighth(h, 1), 255); /* upper eighth */
	} else if (k == 0x95) {
		fill(c, eighth(w, 7), 0, w, h, 255); /* right eighth */
	} else {
		unsigned q = quads[k - 0x96];
		if (q & 1)
			fill(c, 0, 0, xm, ym, 255);
		if (q & 2)
			fill(c, xm, 0, w, ym, 255);
		if (q & 4)
			fill(c, 0, ym, xm, h, 255);
		if (q & 8)
			fill(c, xm, ym, w, h, 255);
	}
}

int box_thin(double underline, int cw, int scale)
{
	int t = (int)lround(underline > 0 ? underline : cw / 10.0);
	return MAX(t, MAX(scale, 1));
}

bool box_covers(uint32_t cp)
{
	return cp >= 0x2500 && cp <= 0x259f;
}

void box_draw(uint32_t cp, int w, int h, int thin, bool bold, uint8_t *alpha)
{
	if (w <= 0 || h <= 0)
		return;
	memset(alpha, 0, (size_t)w * h);
	if (!box_covers(cp))
		return;
	struct canvas c = { alpha, w, h };
	if (cp >= 0x2580) {
		block(&c, cp);
		return;
	}
	/* no stroke wider than the cell, and a double band of three that fits */
	int small = MIN(w, h);
	thin = MAX(1, MIN(thin, small / 2));
	int heavy = MIN(2 * thin, small);
	int light = bold ? heavy : thin;
	int td = MAX(1, MIN(thin, small / 3));
	switch (cp) {
	case 0x2504: case 0x2505: dashes(&c, 3, cp & 1 ? heavy : light, false); return;
	case 0x2506: case 0x2507: dashes(&c, 3, cp & 1 ? heavy : light, true); return;
	case 0x2508: case 0x2509: dashes(&c, 4, cp & 1 ? heavy : light, false); return;
	case 0x250a: case 0x250b: dashes(&c, 4, cp & 1 ? heavy : light, true); return;
	case 0x254c: case 0x254d: dashes(&c, 2, cp & 1 ? heavy : light, false); return;
	case 0x254e: case 0x254f: dashes(&c, 2, cp & 1 ? heavy : light, true); return;
	case 0x256d: arc(&c, light, 1, 1); return;
	case 0x256e: arc(&c, light, -1, 1); return;
	case 0x256f: arc(&c, light, -1, -1); return;
	case 0x2570: arc(&c, light, 1, -1); return;
	case 0x2571: diagonal(&c, light, 1); return;
	case 0x2572: diagonal(&c, light, -1); return;
	case 0x2573: diagonal(&c, light, 1); diagonal(&c, light, -1); return;
	}
	uint8_t a = arms[cp - 0x2500];
	struct lines s = { { 0, light, heavy, td }, a & 3, a >> 2 & 3, a >> 4 & 3, a >> 6 & 3 };
	horizontal(&c, &s, -1);
	horizontal(&c, &s, 1);
	vertical(&c, &s, -1);
	vertical(&c, &s, 1);
}
