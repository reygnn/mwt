/* tests/font_cache.c - the glyph cache keeps to its limits, and output cannot
 * choose glyphs that pile up in its hash table. font.c is built in
 * here with small ones, so that a few hundred glyphs empty the cache many
 * times; this file can then look at the cache itself. Latin glyphs reach the
 * limit in bytes, empty ones from a private use plane, which no font has, the
 * limit in glyphs. A glyph drawn again after that must come back as it was, and
 * under sanitizers (make font_cache) nothing may leak. Needs a font with Latin
 * glyphs, as the image of make evil has. */
#define GLYPH_CACHE_MAX 64
#define GLYPH_CACHE_BYTES (4u << 10)
static unsigned long probes;
#define SLOT_PROBE() (probes++)
#include "../font.c"

#include <stdio.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

struct copy {
	int w, h, left, top;
	uint8_t alpha[1 << 14];
};

static void keep(struct copy *c, const struct glyph *g)
{
	c->w = g->w;
	c->h = g->h;
	c->left = g->left;
	c->top = g->top;
	if ((size_t)g->w * g->h <= sizeof c->alpha)
		memcpy(c->alpha, g->alpha, (size_t)g->w * g->h);
}

static bool same(const struct copy *c, const struct glyph *g)
{
	return c->w == g->w && c->h == g->h && c->left == g->left && c->top == g->top &&
	       !memcmp(c->alpha, g->alpha, (size_t)g->w * g->h);
}

/* Where a fixed multiplier, 2654435761, put a key in a table of 2^BITS: by the
 * low bits as it was, or by the top bits alone. */
enum { BITS = 17 };
static uint32_t fixed_slot(uint32_t key, bool top)
{
	uint32_t p = key * 2654435761u;
	return top ? p >> (32 - BITS) : p & ((1u << BITS) - 1);
}

/* 60000 keys (code point and style, below 2^23) that the fixed multiplier
 * starts in the first 60000 slots of a table of 2^17, the size 64k glyphs grow
 * it to, and 1000 more it starts there too. Each of those walked the run to its
 * end, 30000 steps on average. With the font's seed: a step or two. */
static int hash_flood(struct font *f, bool top)
{
	enum { N = 60000, ASK = 1000 };
	size_t cap = (size_t)1 << BITS;
	struct entry *tab = calloc(cap, sizeof *tab);
	if (!tab)
		return 0;
	int n = 0, ask = 0;
	probes = 0;
	unsigned long before = 0;
	for (uint32_t key = 1; key < 1u << 23 && ask < ASK; key++) {
		if (fixed_slot(key, top) >= N)
			continue;
		if (n < N) {
			struct entry *e = slot(tab, cap, key, f->hash_seed);
			e->used = true;
			e->key = key;
			n++;
			before = probes;
		} else {
			slot(tab, cap, key, f->hash_seed);
			ask++;
		}
	}
	free(tab);
	unsigned long steps = probes - before;
	if (steps > 8 * ASK) {
		printf("FAIL: %d lookups of keys chosen against the %s bits took %lu steps\n", ask,
		       top ? "top" : "low", steps);
		return 1;
	}
	return 0;
}

/* Code points in a row: 16384 from U+4E00 in all four styles fill a table of
 * 2^17 with keys in a row. A random multiplier ran them into long runs for one
 * in 15 or so, up to 83 steps a lookup on average. */
static int hash_dense(struct font *f)
{
	enum { N = 65536 };
	size_t cap = (size_t)1 << BITS;
	struct entry *tab = calloc(cap, sizeof *tab);
	if (!tab)
		return 0;
	for (uint32_t k = 0; k < N; k++) {
		struct entry *e = slot(tab, cap, 0x4e00u << 2 | k, f->hash_seed);
		e->used = true;
		e->key = 0x4e00u << 2 | k;
	}
	probes = 0;
	for (uint32_t k = 0; k < N; k++)
		slot(tab, cap, 0x4e00u << 2 | k, f->hash_seed);
	free(tab);
	if (probes > 2 * N) {
		printf("FAIL: %d lookups of code points in a row took %lu steps\n", N, probes);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	struct font *f = font_open(spec, 0, 1);
	if (!f) {
		fprintf(stderr, "font_cache: no font for '%s'\n", spec);
		return 1;
	}
	static struct copy a, ab;
	keep(&a, font_glyph(f, 'A', STYLE_REGULAR));
	keep(&ab, font_glyph(f, 'A', STYLE_BOLD));
	if (!a.w || (size_t)a.w * a.h > sizeof a.alpha) {
		fprintf(stderr, "font_cache: no usable glyph for A\n");
		return 1;
	}
	size_t most = 0, most_bytes = 0, biggest = 0;
	int emptied[2] = { 0, 0 }, fails = 0;
	/* 0: Latin, all four styles, four times over; 1: empty glyphs */
	for (int phase = 0; phase < 2; phase++)
		for (int round = 0; round < (phase ? 1 : 4); round++)
			for (uint32_t cp = phase ? 0xf0000 : 0x21; cp < (phase ? 0xf0100 : 0x250); cp++) {
				if (cp >= 0x7f && cp < 0xa1)
					continue;
				for (int s = 0; s < (phase ? 1 : 4); s++) {
					size_t before = f->cache_used;
					const struct glyph *g = font_glyph(f, cp, s);
					if (f->cache_used < before)
						emptied[phase]++;
					biggest = MAX(biggest, (size_t)g->w * g->h);
					most = MAX(most, f->cache_used);
					most_bytes = MAX(most_bytes, f->cache_bytes);
				}
			}
	if (most > GLYPH_CACHE_MAX) {
		printf("FAIL: %zu glyphs in the cache, the limit is %d\n", most, GLYPH_CACHE_MAX);
		fails++;
	}
	/* the bytes are looked at before a glyph goes in, so one may go over */
	if (most_bytes > GLYPH_CACHE_BYTES + biggest) {
		printf("FAIL: %zu bytes in the cache, the limit is %u\n", most_bytes, GLYPH_CACHE_BYTES);
		fails++;
	}
	if (!emptied[0] || !emptied[1]) {
		printf("FAIL: the cache was emptied %d times with Latin glyphs, %d with empty ones\n",
		       emptied[0], emptied[1]);
		fails++;
	}
	if (!same(&a, font_glyph(f, 'A', STYLE_REGULAR)) || !same(&ab, font_glyph(f, 'A', STYLE_BOLD))) {
		printf("FAIL: A after the cache was emptied is not the A before\n");
		fails++;
	}
	fails += hash_flood(f, false);
	fails += hash_flood(f, true);
	fails += hash_dense(f);
	font_close(f);
	if (fails)
		return 1;
	printf("font_cache: emptied %d times with Latin glyphs, %d with empty ones, at most %zu "
	       "glyphs and %zu bytes, A as before\n", emptied[0], emptied[1], most, most_bytes);
	return 0;
}
