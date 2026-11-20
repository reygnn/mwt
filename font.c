/* font.c - one monospace font in four styles plus fallback faces for
 * missing glyphs, rendered as 8-bit coverage maps (grayscale AA only) */
#define _POSIX_C_SOURCE 200809L
#include "font.h"

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fallback faces kept open at a time. With more scripts than that, the face used
 * longest ago makes room: glyphs already drawn stay in the cache, which holds
 * bitmaps and no faces, so only a glyph not drawn before loads it again. */
#define MAX_FALLBACK 16

/* The glyph cache is emptied when it holds this many glyphs or bitmap bytes:
 * text with ever new characters, random Unicode, would grow it without end. What
 * a screen shows is far less, and drawn again the next time it is needed. The
 * tests build font.c with smaller ones (tests/font_cache.c). */
#ifndef GLYPH_CACHE_MAX
#define GLYPH_CACHE_MAX 65536
#endif
#ifndef GLYPH_CACHE_BYTES
#define GLYPH_CACHE_BYTES (64u << 20)
#endif

struct face {
	FT_Face ft;
	bool embolden;
	char *path;
	int index;
	double weight; /* fontconfig's, to tell a bold face from one to embolden */
	unsigned long used; /* fallback faces: font.tick when last found */
};

struct entry {
	uint32_t key; /* cp << 2 | style, 0 = empty slot */
	bool used;
	struct glyph g;
};

struct font {
	FT_Library lib;
	FcPattern *base;
	struct face style[4];
	struct face fallback[MAX_FALLBACK];
	int nfallback;
	/* the system's fonts in order of preference, made once on the first
	 * missing glyph, and what all of them cover together */
	bool sorted_tried;
	FcPattern *sort_pat;
	FcFontSet *sorted;
	FcCharSet *cover;
	unsigned long tick;
	double size_pt, size_px;
	int cw, ch, ascent;
	struct entry *cache;
	size_t cache_cap, cache_used;
	size_t cache_bytes; /* of the bitmaps in it */
};

static bool load_face(struct font *f, FcPattern *match, struct face *out)
{
	FcChar8 *file;
	int index = 0;
	FcBool emb = FcFalse;
	FcMatrix *fcm;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		return false;
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	FcPatternGetBool(match, FC_EMBOLDEN, 0, &emb);
	out->weight = FC_WEIGHT_REGULAR;
	FcPatternGetDouble(match, FC_WEIGHT, 0, &out->weight);
	if (FT_New_Face(f->lib, (const char *)file, index, &out->ft))
		return false;
	if (!FT_IS_SCALABLE(out->ft)) {
		FT_Done_Face(out->ft);
		out->ft = NULL;
		return false;
	}
	FT_Set_Char_Size(out->ft, 0, (FT_F26Dot6)lround(f->size_px * 64), 72, 72);
	if (FcPatternGetMatrix(match, FC_MATRIX, 0, &fcm) == FcResultMatch) {
		FT_Matrix m = {
			(FT_Fixed)(fcm->xx * 0x10000), (FT_Fixed)(fcm->xy * 0x10000),
			(FT_Fixed)(fcm->yx * 0x10000), (FT_Fixed)(fcm->yy * 0x10000),
		};
		FT_Set_Transform(out->ft, &m, NULL);
	}
	/* the path tells faces apart in find_fallback, which needs one */
	out->path = strdup((const char *)file);
	if (!out->path) {
		FT_Done_Face(out->ft);
		out->ft = NULL;
		return false;
	}
	out->embolden = emb;
	out->index = index;
	return true;
}

static FcPattern *match_pattern(FcPattern *p)
{
	FcResult res;
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);
	return FcFontMatch(NULL, p, &res);
}

struct font *font_open(const char *spec, double size_pt, int scale)
{
	struct font *f = calloc(1, sizeof *f);
	if (!f || !FcInit() || FT_Init_FreeType(&f->lib)) {
		free(f);
		return NULL;
	}
	f->base = FcNameParse((const FcChar8 *)spec);
	if (!f->base)
		goto fail;
	double px = 0, pt = 11;
	FcPatternGetDouble(f->base, FC_SIZE, 0, &pt);
	if (FcPatternGetDouble(f->base, FC_PIXEL_SIZE, 0, &px) != FcResultMatch)
		px = 0;
	if (size_pt > 0) {
		pt = size_pt;
		px = 0;
	}
	if (px > 0)
		pt = px * 72.0 / 96.0;
	f->size_pt = pt;
	f->size_px = pt * 96.0 / 72.0 * (scale > 0 ? scale : 1);
	FcPatternDel(f->base, FC_SIZE);
	FcPatternDel(f->base, FC_PIXEL_SIZE);
	FcPatternAddDouble(f->base, FC_PIXEL_SIZE, f->size_px);

	/* Bold, italic and bold italic are regular's face with only what they are
	 * for changed: its weight and width otherwise, so a light or condensed
	 * regular gets a light italic and a condensed bold. Not its style name,
	 * which outranks weight and slant in fontconfig: style=Book would give all
	 * four the same face. */
	double rweight = FC_WEIGHT_REGULAR, rwidth = 0;
	int rslant = FC_SLANT_ROMAN;
	for (int i = 0; i < 4; i++) {
		FcPattern *p = FcPatternDuplicate(f->base);
		if (!p) /* FcPatternDel and FcPatternAdd* do not take NULL */
			goto fail;
		FcValue v;
		if (i) {
			FcPatternDel(p, FC_STYLE);
			FcPatternDel(p, FC_WEIGHT);
			FcPatternAddDouble(p, FC_WEIGHT, i & STYLE_BOLD && rweight < FC_WEIGHT_BOLD ?
						  FC_WEIGHT_BOLD : rweight);
			FcPatternDel(p, FC_SLANT);
			FcPatternAddInteger(p, FC_SLANT, i & STYLE_ITALIC ? FC_SLANT_ITALIC : rslant);
			if (rwidth > 0) {
				FcPatternDel(p, FC_WIDTH);
				FcPatternAddDouble(p, FC_WIDTH, rwidth);
			}
		} else {
			/* regular keeps a weight and slant given in the spec */
			if (FcPatternGet(p, FC_WEIGHT, 0, &v) != FcResultMatch)
				FcPatternAddInteger(p, FC_WEIGHT, FC_WEIGHT_REGULAR);
			if (FcPatternGet(p, FC_SLANT, 0, &v) != FcResultMatch)
				FcPatternAddInteger(p, FC_SLANT, FC_SLANT_ROMAN);
		}
		FcPatternAddBool(p, FC_SCALABLE, FcTrue);
		FcPattern *m = match_pattern(p);
		FcPatternDestroy(p);
		bool ok = m && load_face(f, m, &f->style[i]);
		if (ok && i == 0) {
			rweight = f->style[0].weight;
			FcPatternGetDouble(m, FC_WIDTH, 0, &rwidth);
			FcPatternGetInteger(m, FC_SLANT, 0, &rslant);
		}
		if (m)
			FcPatternDestroy(m);
		if (!ok) {
			if (i == 0) {
				fprintf(stderr, "mwt: no scalable font for '%s'\n", spec);
				goto fail;
			}
			/* reuse regular face, synthesize bold */
			if (FT_New_Face(f->lib, f->style[0].path, f->style[0].index,
					&f->style[i].ft))
				goto fail;
			FT_Set_Char_Size(f->style[i].ft, 0, (FT_F26Dot6)lround(f->size_px * 64), 72, 72);
			f->style[i].embolden = f->style[0].embolden ||
					       (i & STYLE_BOLD && rweight < FC_WEIGHT_DEMIBOLD);
			f->style[i].weight = rweight;
			f->style[i].index = f->style[0].index; /* find_fallback compares it */
			f->style[i].path = strdup(f->style[0].path);
			if (!f->style[i].path)
				goto fail; /* font_close closes the face */
		}
	}

	FT_Face r = f->style[0].ft;
	f->ascent = (int)ceil(r->size->metrics.ascender / 64.0);
	int descent = (int)ceil(-r->size->metrics.descender / 64.0);
	f->ch = f->ascent + descent;
	if (r->size->metrics.height / 64 > f->ch)
		f->ch = (int)(r->size->metrics.height / 64);
	f->cw = (int)ceil(r->size->metrics.max_advance / 64.0);
	FT_UInt gi = FT_Get_Char_Index(r, 'M');
	if (gi && !FT_Load_Glyph(r, gi, FT_LOAD_DEFAULT))
		f->cw = (int)lround(r->glyph->advance.x / 64.0);
	if (f->cw < 1)
		f->cw = 1;
	if (f->ch < 1 || f->ascent < 1) { /* FreeType refused the size */
		fprintf(stderr, "mwt: font size %g is out of range\n", pt);
		goto fail;
	}

	f->cache = calloc(1024, sizeof *f->cache);
	if (!f->cache)
		goto fail;
	f->cache_cap = 1024; /* only now: font_close walks cache_cap entries */
	return f;
fail:
	font_close(f);
	return NULL;
}

static void free_face(struct face *fc)
{
	if (fc->ft)
		FT_Done_Face(fc->ft);
	free(fc->path);
}

void font_close(struct font *f)
{
	if (!f)
		return;
	for (size_t i = 0; i < f->cache_cap; i++)
		if (f->cache[i].used)
			free(f->cache[i].g.alpha);
	free(f->cache);
	for (int i = 0; i < 4; i++)
		free_face(&f->style[i]);
	for (int i = 0; i < f->nfallback; i++)
		free_face(&f->fallback[i]);
	if (f->sorted)
		FcFontSetDestroy(f->sorted);
	if (f->cover)
		FcCharSetDestroy(f->cover);
	if (f->sort_pat)
		FcPatternDestroy(f->sort_pat);
	if (f->base)
		FcPatternDestroy(f->base);
	if (f->lib)
		FT_Done_FreeType(f->lib);
	free(f);
}

void font_metrics(const struct font *f, int *cw, int *ch, int *ascent)
{
	*cw = f->cw;
	*ch = f->ch;
	*ascent = f->ascent;
}

double font_size_pt(const struct font *f)
{
	return f->size_pt;
}

/* One FcFontSort for all missing glyphs instead of an FcFontMatch for each:
 * with hundreds of fonts a match took 0.2 ms, and a screen of characters no font
 * has, each new, held up the event loop for seconds. */
static bool sorted_fonts(struct font *f)
{
	if (f->sorted_tried)
		return f->sorted && f->cover;
	f->sorted_tried = true;
	f->sort_pat = FcPatternDuplicate(f->base);
	if (!f->sort_pat) /* FcPatternAdd* and FcConfigSubstitute do not take NULL */
		return false;
	/* the fallback faces in regular: a style=Bold or :bold in the spec put
	 * bold files first, emboldened once more in bold */
	FcPatternDel(f->sort_pat, FC_STYLE);
	FcPatternDel(f->sort_pat, FC_WEIGHT);
	FcPatternDel(f->sort_pat, FC_SLANT);
	FcPatternAddBool(f->sort_pat, FC_SCALABLE, FcTrue);
	FcConfigSubstitute(NULL, f->sort_pat, FcMatchPattern);
	FcDefaultSubstitute(f->sort_pat);
	FcResult res;
	/* trimmed: a font that adds nothing to those before it is left out */
	f->sorted = FcFontSort(NULL, f->sort_pat, FcTrue, &f->cover, &res);
	return f->sorted && f->cover;
}

/* How close the style face i is to style, for one that does not fit: what they
 * share first, then the less it adds. */
static int style_near(int i, int style)
{
	return 4 * __builtin_popcount((unsigned)(i & style)) - __builtin_popcount((unsigned)(i & ~style));
}

/* find or load a face that has cp; NULL if none on the system. The style faces
 * that fit were tried before; one that does not comes last, from the font's
 * own file: U+27BF, only in DejaVu's bold file, would be bold in regular. */
static struct face *find_fallback(struct font *f, uint32_t cp, int style)
{
	struct face *other = NULL;
	for (int i = 0; i < f->nfallback; i++)
		if (FT_Get_Char_Index(f->fallback[i].ft, cp)) {
			f->fallback[i].used = ++f->tick;
			return &f->fallback[i];
		}
	if (!sorted_fonts(f) || !FcCharSetHasChar(f->cover, cp))
		return NULL;
	for (int k = 0; k < f->sorted->nfont; k++) {
		FcCharSet *cs;
		if (FcPatternGetCharSet(f->sorted->fonts[k], FC_CHARSET, 0, &cs) != FcResultMatch ||
		    !FcCharSetHasChar(cs, cp))
			continue;
		FcPattern *m = FcFontRenderPrepare(NULL, f->sort_pat, f->sorted->fonts[k]);
		FcChar8 *file;
		int index = 0;
		if (!m || FcPatternGetString(m, FC_FILE, 0, &file) != FcResultMatch) {
			if (m)
				FcPatternDestroy(m);
			continue;
		}
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		/* Open already: a fallback face was tried above and lacks cp, but
		 * bold or italic may have what regular lacks. */
		bool open = false;
		for (int i = 0; i < 4; i++)
			if (!strcmp(f->style[i].path, (const char *)file) && f->style[i].index == index) {
				open = true;
				if (FT_Get_Char_Index(f->style[i].ft, cp) &&
				    (!other || style_near(i, style) > style_near((int)(other - f->style), style)))
					other = &f->style[i];
			}
		for (int i = 0; i < f->nfallback; i++)
			open |= !strcmp(f->fallback[i].path, (const char *)file) &&
				f->fallback[i].index == index;
		if (open) {
			FcPatternDestroy(m);
			continue;
		}
		struct face nf = { 0 };
		bool ok = load_face(f, m, &nf);
		FcPatternDestroy(m);
		if (!ok)
			continue; /* not scalable, or unreadable: the next may do */
		if (!FT_Get_Char_Index(nf.ft, cp)) {
			free_face(&nf);
			continue;
		}
		int slot = f->nfallback;
		if (slot == MAX_FALLBACK) {
			slot = 0;
			for (int i = 1; i < MAX_FALLBACK; i++)
				if (f->fallback[i].used < f->fallback[slot].used)
					slot = i;
			free_face(&f->fallback[slot]);
		} else {
			f->nfallback++;
		}
		nf.embolden = false;
		nf.used = ++f->tick;
		f->fallback[slot] = nf;
		return &f->fallback[slot];
	}
	return other;
}

/* The face cp is drawn from in style, NULL if none has it; *embolden says
 * whether to make it bold. */
static struct face *face_for(struct font *f, uint32_t cp, int style, bool *embolden)
{
	/* Bold and italic faces often cover less than regular (DejaVu Sans Mono
	 * Oblique lacks hundreds): the font's own faces that fit, before the
	 * system's fonts, whatever was drawn before. For bold italic the bold face
	 * first, then the italic one, then regular: real bold outlines rather than
	 * synthesized ones, the slant lost either way. */
	static const int order[4][4] = {
		{ STYLE_REGULAR, -1 },
		{ STYLE_BOLD, STYLE_REGULAR, -1 },
		{ STYLE_ITALIC, STYLE_REGULAR, -1 },
		{ STYLE_BOLD_ITALIC, STYLE_BOLD, STYLE_ITALIC, STYLE_REGULAR },
	};
	struct face *fc = NULL;
	for (int k = 0; k < 4 && order[style][k] >= 0 && !fc; k++)
		if (FT_Get_Char_Index(f->style[order[style][k]].ft, cp))
			fc = &f->style[order[style][k]];
	if (!fc)
		fc = find_fallback(f, cp, style);
	if (!fc)
		return NULL;
	/* bold synthesized for a face that is not bold itself */
	bool bold = style & STYLE_BOLD;
	*embolden = fc->embolden || (bold && fc->weight < FC_WEIGHT_DEMIBOLD);
	return fc;
}

static void render(struct font *f, uint32_t cp, int style, struct glyph *g)
{
	memset(g, 0, sizeof *g);
	bool embolden;
	struct face *fc = face_for(f, cp, style, &embolden);
	FT_UInt gi = fc ? FT_Get_Char_Index(fc->ft, cp) : 0;
	if (!gi || FT_Load_Glyph(fc->ft, gi, FT_LOAD_TARGET_LIGHT))
		return;
	FT_GlyphSlot s = fc->ft->glyph;
	if (embolden)
		FT_GlyphSlot_Embolden(s);
	if (FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL))
		return;
	FT_Bitmap *b = &s->bitmap;
	if (!b->width || !b->rows)
		return;
	g->w = b->width;
	g->h = b->rows;
	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->alpha = malloc((size_t)g->w * g->h);
	if (!g->alpha) {
		g->w = g->h = 0;
		return;
	}
	for (int y = 0; y < g->h; y++) {
		const uint8_t *src = b->buffer + y * b->pitch;
		uint8_t *dst = g->alpha + y * g->w;
		if (b->pixel_mode == FT_PIXEL_MODE_MONO)
			for (int x = 0; x < g->w; x++)
				dst[x] = (src[x >> 3] >> (7 - (x & 7)) & 1) ? 255 : 0;
		else if (b->pixel_mode == FT_PIXEL_MODE_GRAY)
			memcpy(dst, src, g->w);
		else
			memset(dst, 0, g->w);
	}
}

static struct entry *slot(struct entry *tab, size_t cap, uint32_t key)
{
	size_t i = (key * 2654435761u) & (cap - 1);
	while (tab[i].used && tab[i].key != key)
		i = (i + 1) & (cap - 1);
	return &tab[i];
}

static void cache_empty(struct font *f)
{
	for (size_t i = 0; i < f->cache_cap; i++)
		if (f->cache[i].used)
			free(f->cache[i].g.alpha);
	memset(f->cache, 0, f->cache_cap * sizeof *f->cache);
	f->cache_used = f->cache_bytes = 0;
}

const struct glyph *font_glyph(struct font *f, uint32_t cp, int style)
{
	uint32_t key = cp << 2 | (style & 3);
	struct entry *e = slot(f->cache, f->cache_cap, key);
	if (e->used)
		return &e->g;
	if (f->cache_used >= GLYPH_CACHE_MAX || f->cache_bytes >= GLYPH_CACHE_BYTES) {
		cache_empty(f);
		e = slot(f->cache, f->cache_cap, key);
	}
	if ((f->cache_used + 1) * 10 > f->cache_cap * 7) {
		size_t ncap = f->cache_cap * 2;
		struct entry *n = calloc(ncap, sizeof *n);
		if (n) {
			for (size_t i = 0; i < f->cache_cap; i++)
				if (f->cache[i].used)
					*slot(n, ncap, f->cache[i].key) = f->cache[i];
			free(f->cache);
			f->cache = n;
			f->cache_cap = ncap;
			e = slot(f->cache, f->cache_cap, key);
		} else if (f->cache_used + 1 >= f->cache_cap) {
			/* no memory to grow, and a full table would leave slot()
			 * without a free place: draw nothing for this one */
			static const struct glyph none;
			return &none;
		}
	}
	e->used = true;
	e->key = key;
	render(f, cp, style & 3, &e->g);
	f->cache_used++;
	f->cache_bytes += (size_t)e->g.w * e->g.h;
	return &e->g;
}
