/* tests/font_fallback.c - glyphs of fallback faces (6.2 and 6.6 of the review
 * of 2026-11-18). Each is drawn inside its one or two cells: one too large
 * smaller and centred, one that fits at its own size; the faces keep their size
 * after a glyph was drawn smaller, and the font's own faces are not fitted.
 * A monospaced face that has a character draws it, also where a proportional
 * face opened before has it too. font.c is built in so that this file can ask
 * which face drew what. Needs the fonts of make fonts: NanumGothic and Droid
 * Sans Fallback proportional, D2Coding monospaced, Symbola for emoji. */
#include "../font.c"

#include <stdio.h>

FT_Error __real_FT_New_Face(FT_Library, const char *, FT_Long, FT_Face *);

/* faces opened from a file FreeType cannot draw from: Noto Color Emoji, which
 * fontconfig calls monospaced and scalable, is bitmaps only */
static int unusable;

FT_Error __wrap_FT_New_Face(FT_Library lib, const char *path, FT_Long index, FT_Face *face)
{
	FT_Error err = __real_FT_New_Face(lib, path, index, face);
	unusable += !err && !FT_IS_SCALABLE(*face);
	return err;
}

char *__real_strdup(const char *);

/* a strdup of a path with this in it fails, counted in refused */
static const char *refuse;
static int refused;

char *__wrap_strdup(const char *s)
{
	if (refuse && strstr(s, refuse)) {
		refused++;
		return NULL;
	}
	return __real_strdup(s);
}

static int fails;

#define FAIL(...) do { if (fails++ < 20) printf("FAIL: " __VA_ARGS__); } while (0)

static bool is_fallback(struct font *f, struct face *fc)
{
	return fc >= f->fallback && fc < f->fallback + MAX_FALLBACK;
}

static const struct range {
	uint32_t from, to;
	bool wide;
} ranges[] = {
	{ 0x0900, 0x097f, false }, /* Devanagari */
	{ 0x0e00, 0x0e7f, false }, /* Thai */
	{ 0x1200, 0x137f, false }, /* Ethiopic */
	{ 0x2460, 0x24ff, false }, /* ① and friends, NanumGothic's are 14 px in 9 */
	{ 0x3040, 0x30ff, true },  /* kana */
	{ 0x4e00, 0x4fff, true },  /* CJK */
	{ 0xac00, 0xadff, true },  /* Hangul */
	{ 0xfdf0, 0xfdfd, false }, /* U+FDFD, about 12 cells wide */
	{ 0x1f300, 0x1f64f, true }, /* emoji */
};

struct raw {
	int w, h, left, top, adv;
};

/* the bitmap cp has in fc at the font's size, unfitted */
static bool raw(struct face *fc, uint32_t cp, bool emb, struct raw *r)
{
	FT_UInt gi = FT_Get_Char_Index(fc->ft, cp);
	FT_GlyphSlot s = gi ? load(fc, gi, emb) : NULL;
	if (!s)
		return false;
	*r = (struct raw){ (int)s->bitmap.width, (int)s->bitmap.rows, s->bitmap_left,
			   s->bitmap_top, (int)lround(s->advance.x / 64.0) };
	return r->w && r->h;
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* glyphs drawn smaller for their height, at all sizes: at some none are */
static int all_tall;

static void fit_at(double pt, int scale)
{
	struct font *f = font_open("monospace", pt, scale);
	if (!f) {
		FAIL("no font at %g pt, scale %d\n", pt, scale);
		return;
	}
	int checked = 0, shrunk = 0, moved = 0, tall = 0;
	unusable = 0;
	for (size_t r = 0; r < sizeof ranges / sizeof *ranges; r++)
		for (uint32_t cp = ranges[r].from; cp <= ranges[r].to; cp++)
			for (int st = 0; st < 4; st++) {
				bool emb;
				struct face *fc = face_for(f, cp, st, &emb);
				struct raw rr;
				if (!fc || !is_fallback(f, fc) || !raw(fc, cp, emb, &rr))
					continue;
				int rw = rr.w, rh = rr.h;
				bool wide = ranges[r].wide;
				int W = (wide ? 2 : 1) * f->cw;
				const struct glyph *g = font_glyph(f, cp, st, wide);
				/* none only where the glyph in its cells would be less
				 * than a pixel high or wide: U+FDFD at 6 pt */
				if (!g->w) {
					if ((double)rh * W / rw >= 1.5 && (double)rw * f->ch / rh >= 1.5)
						FAIL("%g pt, scale %d: U+%04X not drawn\n", pt, scale,
						     (unsigned)cp);
					continue;
				}
				checked++;
				int y = f->ascent - g->top; /* from the top of the row */
				if (g->left < 0 || g->left + g->w > W || y < 0 || y + g->h > f->ch)
					FAIL("%g pt, scale %d: U+%04X at %d,%d, %dx%d, outside %dx%d\n", pt,
					     scale, (unsigned)cp, g->left, y, g->w, g->h, W, f->ch);
				if (rw > W || rh > f->ch) {
					shrunk++;
					/* down only if too tall; else on the baseline,
					 * which keeps its ink at the scaled height */
					bool down = rh > f->ch;
					tall += down;
					/* below the baseline as much as at its own size,
					 * scaled; hinting may move it a pixel or two, a
					 * little more far from the baseline.
					 * Clamped into the row, it may not be. */
					double below = (double)(rh - rr.top) * g->h / rh;
					bool clamped = g->top == f->ascent || g->top - g->h == f->ascent - f->ch;
					if (!down && !clamped && fabs((g->h - g->top) - below) > 2 + fabs(below) / 8)
						FAIL("%g pt, scale %d: U+%04X drawn smaller %d px below the "
						     "baseline, not about %.1f\n", pt, scale, (unsigned)cp,
						     g->h - g->top, below);
					if (abs(2 * g->left - (W - g->w)) > 1 ||
					    (down && abs(2 * y - (f->ch - g->h)) > 1))
						FAIL("%g pt, scale %d: U+%04X drawn smaller at %d,%d, %dx%d, "
						     "not centred in %dx%d\n", pt, scale, (unsigned)cp, g->left,
						     y, g->w, g->h, W, f->ch);
				} else if (g->w != rw || g->h != rh) {
					FAIL("%g pt, scale %d: U+%04X fits at %dx%d, drawn %dx%d\n", pt,
					     scale, (unsigned)cp, rw, rh, g->w, g->h);
				} else {
					/* where a monospaced font would put it: its advance
					 * centred, the ink kept in the cells */
					int left = clamp(rr.left + (rr.adv > 0 ? (W - rr.adv) / 2 : 0), 0, W - rw);
					int top = clamp(rr.top, rh + f->ascent - f->ch, f->ascent);
					if (g->left != left || g->top != top)
						FAIL("%g pt, scale %d: U+%04X at %d,%d, not %d,%d\n", pt, scale,
						     (unsigned)cp, g->left, g->top, left, top);
					moved += rr.adv != W;
				}
			}
	if (checked < 1000 || shrunk < 100 || moved < 100 || shrunk - tall < 100)
		FAIL("%g pt, scale %d: only %d glyphs from fallback faces, %d too large (%d too "
		     "tall), %d with another advance\n", pt, scale, checked, shrunk, tall, moved);
	/* tried once, not at each emoji */
	if (unusable > 1)
		FAIL("%g pt, scale %d: a face that cannot be drawn from opened %d times\n", pt,
		     scale, unusable);
	/* the size the faces are set to after all that */
	for (int i = 0; i < f->nfallback; i++)
		if (f->fallback[i].ft->size->metrics.y_ppem != f->style[0].ft->size->metrics.y_ppem)
			FAIL("%g pt, scale %d: %s left at %d ppem, not %d\n", pt, scale,
			     f->fallback[i].path, f->fallback[i].ft->size->metrics.y_ppem,
			     f->style[0].ft->size->metrics.y_ppem);
	/* the font's own glyphs as they are, also in a wide cell */
	for (uint32_t cp = 0x21; cp < 0x7f; cp++) {
		struct glyph a = *font_glyph(f, cp, STYLE_REGULAR, false);
		const struct glyph *b = font_glyph(f, cp, STYLE_REGULAR, true);
		if (a.w != b->w || a.h != b->h || a.left != b->left || a.top != b->top)
			FAIL("%g pt: '%c' moved in a wide cell\n", pt, (int)cp);
	}
	font_close(f);
	all_tall += tall;
	printf("%g pt, scale %d: %d glyphs, %d drawn smaller (%d too tall), %d centred by their "
	       "advance\n", pt, scale, checked, shrunk, tall, moved);
}

/* fonts fontconfig calls monospaced that mwt can load: the oracle */
static FcFontSet *monos;

static void find_monos(struct font *f)
{
	FcPattern *p = FcPatternCreate();
	FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_SPACING, FC_CHARSET, (char *)NULL);
	FcFontSet *all = FcFontList(NULL, p, os);
	monos = FcFontSetCreate();
	for (int k = 0; all && k < all->nfont; k++) {
		FcChar8 *file;
		int index = 0;
		FcPattern *q = all->fonts[k];
		if (!is_mono(q) || FcPatternGetString(q, FC_FILE, 0, &file) != FcResultMatch)
			continue;
		FcPatternGetInteger(q, FC_INDEX, 0, &index);
		bool style = false;
		for (int i = 0; i < 4; i++)
			style |= !strcmp(f->style[i].path, (const char *)file) && f->style[i].index == index;
		FT_Face ft;
		if (style || FT_New_Face(f->lib, (const char *)file, index, &ft))
			continue;
		bool scalable = FT_IS_SCALABLE(ft);
		FT_Done_Face(ft);
		if (scalable) {
			printf("monospaced: %s\n", (const char *)file);
			FcFontSetAdd(monos, FcPatternDuplicate(q));
		}
	}
	if (all)
		FcFontSetDestroy(all);
	FcObjectSetDestroy(os);
	FcPatternDestroy(p);
}

static bool mono_has(uint32_t cp)
{
	for (int k = 0; k < monos->nfont; k++) {
		FcCharSet *cs;
		if (FcPatternGetCharSet(monos->fonts[k], FC_CHARSET, 0, &cs) == FcResultMatch &&
		    FcCharSetHasChar(cs, cp))
			return true;
	}
	return false;
}

static void mono_order(const char *name, const uint32_t (*walk)[2], int n)
{
	struct font *f = font_open("monospace", 11, 1);
	if (!f) {
		FAIL("no font\n");
		return;
	}
	if (!monos)
		find_monos(f);
	int expect = 0, contest = 0;
	for (int w = 0; w < n; w++) {
		uint32_t from = walk[w][0], to = walk[w][1];
		int step = from <= to ? 3 : -3;
		for (int64_t cp = from; step > 0 ? cp <= to : cp >= to; cp += step) {
			bool emb;
			if (FT_Get_Char_Index(f->style[0].ft, (uint32_t)cp))
				continue;
			bool want = mono_has((uint32_t)cp);
			/* a proportional face open already that has it */
			bool prop = false;
			for (int i = 0; i < f->nfallback; i++)
				prop |= !f->fallback[i].mono && FT_Get_Char_Index(f->fallback[i].ft, (uint32_t)cp);
			struct face *fc = face_for(f, (uint32_t)cp, STYLE_REGULAR, &emb);
			if (!fc || !want)
				continue;
			expect++;
			contest += prop;
			if (!is_fallback(f, fc) || !fc->mono)
				FAIL("%s: U+%04X from %s, a monospaced font has it\n", name,
				     (unsigned)cp, fc->path);
		}
	}
	printf("%s: %d from monospaced fonts, %d with a proportional face open that has them\n",
	       name, expect, contest);
	/* else the order would not matter */
	if (expect < 1000 || contest < 100)
		FAIL("%s: only %d characters a monospaced font has, %d contested\n", name,
		     expect, contest);
	font_close(f);
}

/* A face that did not load for want of memory is tried again, unlike one that
 * cannot load: D2Coding, regular and bold refused for 한, draws 핝 after it. */
static void mono_oom(void)
{
	struct font *f = font_open("monospace", 11, 1);
	if (!f) {
		FAIL("no font\n");
		return;
	}
	bool emb;
	refuse = "/D2Coding";
	struct face *a = face_for(f, 0xd55c, STYLE_REGULAR, &emb);
	if (!refused)
		FAIL("oom: D2Coding not loaded for U+D55C\n");
	refuse = NULL;
	struct face *b = face_for(f, 0xd55d, STYLE_REGULAR, &emb);
	if (!a || strstr(a->path, "/D2Coding"))
		FAIL("oom: U+D55C from %s with D2Coding refused\n", a ? a->path : "nothing");
	if (!b || !strstr(b->path, "/D2Coding"))
		FAIL("oom: U+D55D from %s after D2Coding was refused\n", b ? b->path : "nothing");
	font_close(f);
}

static char *family_file(const char *family)
{
	FcPattern *p = FcNameParse((const FcChar8 *)family);
	FcPattern *m = p ? match_pattern(p) : NULL;
	FcChar8 *file;
	char *path = m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch ?
			     strdup((const char *)file) : NULL;
	if (m)
		FcPatternDestroy(m);
	if (p)
		FcPatternDestroy(p);
	return path;
}

/* A monospaced font with nothing a proportional one ranked before it lacks, as
 * Noto Sans Mono CJK behind Noto Sans CJK: a trimmed FcFontSort leaves it out.
 * sub is D2Coding Bold cut to the hanja Droid Sans Fallback has, ranked after
 * it as bold; the system's fonts are left out, so these three are all there
 * is. Last: the configuration stays. */
static void mono_covered(const char *sub)
{
	char *own = family_file("DejaVu Sans Mono"), *prop = family_file("Droid Sans Fallback");
	FcConfig *c = FcConfigCreate();
	if (!own || !prop || !c || !FcConfigAppFontAddFile(c, (const FcChar8 *)own) ||
	    !FcConfigAppFontAddFile(c, (const FcChar8 *)prop) ||
	    !FcConfigAppFontAddFile(c, (const FcChar8 *)sub) || !FcConfigSetCurrent(c)) {
		FAIL("no configuration of %s, %s and %s\n", own, prop, sub);
		goto out;
	}
	struct font *f = font_open("DejaVu Sans Mono", 11, 1);
	if (!f || !strstr(f->style[0].path, "DejaVuSansMono")) {
		FAIL("no DejaVu Sans Mono in the cut down configuration\n");
		goto close;
	}
	if (!sorted_fonts(f)) {
		FAIL("no sorted list\n");
		goto close;
	}
	/* else a trimmed list would do as well */
	FcResult res;
	FcFontSet *trimmed = FcFontSort(NULL, f->sort_pat, FcTrue, NULL, &res);
	for (int k = 0; trimmed && k < trimmed->nfont; k++) {
		FcChar8 *file;
		if (FcPatternGetString(trimmed->fonts[k], FC_FILE, 0, &file) == FcResultMatch &&
		    !strcmp((const char *)file, sub))
			FAIL("%s is in the trimmed list: the test shows nothing\n", sub);
	}
	if (trimmed)
		FcFontSetDestroy(trimmed);
	int n = 0;
	FcCharSet *cs = NULL;
	for (int k = 0; k < f->sorted->nfont; k++) {
		FcChar8 *file;
		if (FcPatternGetString(f->sorted->fonts[k], FC_FILE, 0, &file) == FcResultMatch &&
		    !strcmp((const char *)file, sub))
			FcPatternGetCharSet(f->sorted->fonts[k], FC_CHARSET, 0, &cs);
	}
	for (uint32_t cp = 0x4e00; cs && cp <= 0x9fff; cp++) {
		if (!FcCharSetHasChar(cs, cp))
			continue;
		bool emb;
		struct face *fc = face_for(f, cp, STYLE_REGULAR, &emb);
		n++;
		if (!fc || strcmp(fc->path, sub))
			FAIL("covered: U+%04X from %s, not %s\n", (unsigned)cp,
			     fc ? fc->path : "nothing", sub);
	}
	printf("covered: %d hanja from %s\n", n, sub);
	if (n < 1000)
		FAIL("covered: only %d hanja in %s\n", n, sub);
close:
	font_close(f);
out:
	if (c)
		FcConfigDestroy(c);
	free(own);
	free(prop);
}

int main(int argc, char **argv)
{
	static const double pts[] = { 6, 11, 20 };
	for (int i = 0; i < 3; i++)
		for (int scale = 1; scale <= 2; scale++)
			fit_at(pts[i], scale);
	if (all_tall < 5)
		FAIL("only %d glyphs too tall for their row\n", all_tall);
	/* CJK first: hanzi D2Coding lacks open Droid Sans Fallback or NanumGothic,
	 * which have Hangul too */
	static const uint32_t cjk_first[][2] = { { 0x9fff, 0x4e00 }, { 0xac00, 0xd7a3 } };
	static const uint32_t hangul_first[][2] = { { 0xac00, 0xd7a3 }, { 0x4e00, 0x9fff } };
	mono_order("CJK first", cjk_first, 2);
	mono_order("Hangul first", hangul_first, 2);
	mono_oom();
	if (monos)
		FcFontSetDestroy(monos);
	monos = NULL;
	/* made by tests/fonts-run.sh */
	if (argc > 1)
		mono_covered(argv[1]);
	else
		FAIL("usage: font_fallback MONO_COVERED.ttf\n");
	if (fails)
		printf("%d failures\n", fails);
	else
		printf("PASS\n");
	return fails != 0;
}
