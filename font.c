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

#define MAX_FALLBACK 16

struct face {
	FT_Face ft;
	bool embolden;
	char *path;
	int index;
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
	double size_pt, size_px;
	int cw, ch, ascent;
	struct entry *cache;
	size_t cache_cap, cache_used;
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
	out->embolden = emb;
	out->path = strdup((const char *)file);
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

	static const struct { int weight, slant; } st[4] = {
		{ FC_WEIGHT_REGULAR, FC_SLANT_ROMAN },
		{ FC_WEIGHT_BOLD, FC_SLANT_ROMAN },
		{ FC_WEIGHT_REGULAR, FC_SLANT_ITALIC },
		{ FC_WEIGHT_BOLD, FC_SLANT_ITALIC },
	};
	for (int i = 0; i < 4; i++) {
		FcPattern *p = FcPatternDuplicate(f->base);
		FcValue v;
		/* regular keeps a weight/slant given in the spec */
		if (i || FcPatternGet(p, FC_WEIGHT, 0, &v) != FcResultMatch) {
			FcPatternDel(p, FC_WEIGHT);
			FcPatternAddInteger(p, FC_WEIGHT, st[i].weight);
		}
		if (i || FcPatternGet(p, FC_SLANT, 0, &v) != FcResultMatch) {
			FcPatternDel(p, FC_SLANT);
			FcPatternAddInteger(p, FC_SLANT, st[i].slant);
		}
		FcPatternAddBool(p, FC_SCALABLE, FcTrue);
		FcPattern *m = match_pattern(p);
		FcPatternDestroy(p);
		bool ok = m && load_face(f, m, &f->style[i]);
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
			f->style[i].embolden = st[i].weight == FC_WEIGHT_BOLD;
			f->style[i].path = strdup(f->style[0].path);
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

	f->cache_cap = 1024;
	f->cache = calloc(f->cache_cap, sizeof *f->cache);
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

/* find or load a face that has cp; NULL if none on the system */
static struct face *find_fallback(struct font *f, uint32_t cp)
{
	for (int i = 0; i < f->nfallback; i++)
		if (FT_Get_Char_Index(f->fallback[i].ft, cp))
			return &f->fallback[i];
	if (f->nfallback >= MAX_FALLBACK)
		return NULL;

	FcPattern *p = FcPatternDuplicate(f->base);
	FcCharSet *cs = FcCharSetCreate();
	FcCharSetAddChar(cs, cp);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddBool(p, FC_SCALABLE, FcTrue);
	FcCharSetDestroy(cs);
	FcPattern *m = match_pattern(p);
	FcPatternDestroy(p);
	if (!m)
		return NULL;
	struct face *res = NULL;
	FcCharSet *mcs;
	FcChar8 *file;
	if (FcPatternGetCharSet(m, FC_CHARSET, 0, &mcs) == FcResultMatch &&
	    FcCharSetHasChar(mcs, cp) &&
	    FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
		bool dup = false;
		for (int i = 0; i < 4; i++)
			dup |= !strcmp(f->style[i].path, (const char *)file);
		for (int i = 0; i < f->nfallback; i++)
			dup |= !strcmp(f->fallback[i].path, (const char *)file);
		struct face *fc = &f->fallback[f->nfallback];
		if (!dup && load_face(f, m, fc) && FT_Get_Char_Index(fc->ft, cp)) {
			fc->embolden = false;
			res = fc;
			f->nfallback++;
		} else if (!dup && fc->ft) {
			free_face(fc);
			memset(fc, 0, sizeof *fc);
		}
	}
	FcPatternDestroy(m);
	return res;
}

static void render(struct font *f, uint32_t cp, int style, struct glyph *g)
{
	memset(g, 0, sizeof *g);
	struct face *fc = &f->style[style];
	bool bold = style == STYLE_BOLD || style == STYLE_BOLD_ITALIC;
	FT_UInt gi = FT_Get_Char_Index(fc->ft, cp);
	if (!gi && (fc = find_fallback(f, cp)))
		gi = FT_Get_Char_Index(fc->ft, cp);
	if (!gi || FT_Load_Glyph(fc->ft, gi, FT_LOAD_TARGET_LIGHT))
		return;
	FT_GlyphSlot s = fc->ft->glyph;
	if (fc->embolden || (bold && fc != &f->style[style]))
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

const struct glyph *font_glyph(struct font *f, uint32_t cp, int style)
{
	uint32_t key = cp << 2 | (style & 3);
	struct entry *e = slot(f->cache, f->cache_cap, key);
	if (e->used)
		return &e->g;
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
		}
	}
	e->used = true;
	e->key = key;
	render(f, cp, style & 3, &e->g);
	f->cache_used++;
	return &e->g;
}
