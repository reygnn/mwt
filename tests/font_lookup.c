/* tests/font_lookup.c - glyphs no open face has cost one FcFontSort in all, not
 * an FcFontMatch each: a screen of new ones held up the event loop for seconds
 * with many fonts installed. font.c is built in, with FcFontMatch and FcFontSort
 * counted through --wrap. What the system covers must still be drawn. And out of
 * memory once, a failed FcFontSort or a glyph's bitmap (malloc through --wrap)
 * leaves the glyph blank only that time. Needs a font with Latin glyphs and
 * others for the rest; make fonts runs it. */
#define SORT_RETRY_MS 0
#include "../font.c"

#include <stdio.h>
#include <time.h>

static int matches, sorts;
static bool fail_sort, fail_malloc; /* the next one fails */

void *__real_malloc(size_t n);
void *__wrap_malloc(size_t n)
{
	if (fail_malloc) {
		fail_malloc = false;
		return NULL;
	}
	return __real_malloc(n);
}

FcPattern *__real_FcFontMatch(FcConfig *c, FcPattern *p, FcResult *r);
FcPattern *__wrap_FcFontMatch(FcConfig *c, FcPattern *p, FcResult *r)
{
	matches++;
	return __real_FcFontMatch(c, p, r);
}

FcFontSet *__real_FcFontSort(FcConfig *c, FcPattern *p, FcBool trim, FcCharSet **cs, FcResult *r);
FcFontSet *__wrap_FcFontSort(FcConfig *c, FcPattern *p, FcBool trim, FcCharSet **cs, FcResult *r)
{
	sorts++;
	if (fail_sort) {
		fail_sort = false;
		*r = FcResultOutOfMemory;
		return NULL;
	}
	return __real_FcFontSort(c, p, trim, cs, r);
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	struct font *f = font_open(spec, 0, 1);
	if (!f) {
		fprintf(stderr, "font_lookup: no font for '%s'\n", spec);
		return 1;
	}
	/* what the system covers, asked here so as not to use font.c's own */
	FcPattern *p = FcNameParse((const FcChar8 *)spec);
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	FcCharSet *cover = NULL;
	FcResult res;
	FcFontSet *all = __real_FcFontSort(NULL, p, FcTrue, &cover, &res);
	matches = sorts = 0;
	int fails = 0, covered = 0, drawn = 0;
	double t0 = now();
	/* a private use plane no font has, then scripts some font may have */
	static const uint32_t ranges[][2] = {
		{ 0xf0000, 0xf0800 }, { 0x370, 0x400 }, { 0x4e00, 0x4f00 },
		{ 0x1f600, 0x1f650 }, { 0x0e00, 0x0e60 }, { 0x2800, 0x2900 },
	};
	for (size_t k = 0; k < sizeof ranges / sizeof ranges[0]; k++)
		for (uint32_t cp = ranges[k][0]; cp < ranges[k][1]; cp++)
			for (int s = 0; s < 4; s++) {
				const struct glyph *g = font_glyph(f, cp, s);
				if (s || cp >= 0xf0000)
					continue;
				FcChar32 c = cp;
				if (cover && FcCharSetHasChar(cover, c)) {
					covered++;
					drawn += g->w > 0;
				}
			}
	double dt = now() - t0;
	if (matches || sorts > 1) {
		printf("FAIL: %d FcFontMatch and %d FcFontSort for the missing glyphs, "
		       "expected none and one\n", matches, sorts);
		fails++;
	}
	/* a font may claim a character and draw nothing for it, a space say */
	if (drawn < covered * 9 / 10) {
		printf("FAIL: of %d characters the system covers %d are drawn\n", covered, drawn);
		fails++;
	}
	font_close(f);
	int lookup_sorts = sorts;

	/* a covered character no style face has, from a fallback face */
	uint32_t other = 0;
	f = font_open(spec, 0, 1);
	for (size_t k = 1; f && !other && k < sizeof ranges / sizeof ranges[0]; k++)
		for (uint32_t cp = ranges[k][0]; !other && cp < ranges[k][1]; cp++)
			if (cover && FcCharSetHasChar(cover, cp) &&
			    !FT_Get_Char_Index(f->style[STYLE_REGULAR].ft, cp))
				other = cp;
	if (!other) {
		printf("FAIL: no character from a fallback face to fail the lookup with\n");
		fails++;
	} else {
		fail_sort = true;
		bool blank = !font_glyph(f, other, STYLE_REGULAR)->w;
		bool drawn = font_glyph(f, other, STYLE_REGULAR)->w > 0;
		if (!blank || !drawn) {
			printf("FAIL: U+%04X with FcFontSort failing once: %s the first time, %s the second\n",
			       (unsigned)other, blank ? "blank" : "drawn", drawn ? "drawn" : "blank");
			fails++;
		}
	}
	if (f) {
		fail_malloc = true;
		bool blank = !font_glyph(f, 'A', STYLE_BOLD)->w;
		bool drawn = font_glyph(f, 'A', STYLE_BOLD)->w > 0;
		if (!blank || !drawn) {
			printf("FAIL: A with its bitmap failing once: %s the first time, %s the second\n",
			       blank ? "blank" : "drawn", drawn ? "drawn" : "blank");
			fails++;
		}
		font_close(f);
	}
	if (all)
		FcFontSetDestroy(all);
	if (cover)
		FcCharSetDestroy(cover);
	FcPatternDestroy(p);
	if (fails)
		return 1;
	printf("font_lookup: %d FcFontSort, %d of %d covered characters drawn, all in %.2f s; "
	       "after a failed FcFontSort and a failed bitmap drawn the next time\n",
	       lookup_sorts, drawn, covered, dt);
	return 0;
}
