/* tests/font_cache.c - the glyph cache keeps to its limits. font.c is built in
 * here with small ones, so that a few hundred glyphs empty the cache many
 * times; this file can then look at the cache itself. Latin glyphs reach the
 * limit in bytes, empty ones from a private use plane, which no font has, the
 * limit in glyphs. A glyph drawn again after that must come back as it was, and
 * under sanitizers (make font_cache) nothing may leak. Needs a font with Latin
 * glyphs, as the image of make evil has. */
#define GLYPH_CACHE_MAX 64
#define GLYPH_CACHE_BYTES (4u << 10)
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
	font_close(f);
	if (fails)
		return 1;
	printf("font_cache: emptied %d times with Latin glyphs, %d with empty ones, at most %zu "
	       "glyphs and %zu bytes, A as before\n", emptied[0], emptied[1], most, most_bytes);
	return 0;
}
