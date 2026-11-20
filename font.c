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
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

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

/* A failed FcFontSort, out of memory, is tried again after this long, not at
 * every missing glyph: each one can take milliseconds. tests/font_lookup.c
 * sets 0. */
/* How glyphs are loaded, and M for the cell width: the same, as FT_LOAD_DEFAULT
 * hints the advance another way (17 pixels for DejaVu's 18 at 11 pt and scale 2). */
#define LOAD_FLAGS FT_LOAD_TARGET_LIGHT

/* tests/font_cache.c counts the steps of slot() with it */
#ifndef SLOT_PROBE
#define SLOT_PROBE() ((void)0)
#endif

#ifndef SORT_RETRY_MS
#define SORT_RETRY_MS 1000
#endif

struct face {
	FT_Face ft;
	bool embolden;
	char *path;
	int index;
	double weight; /* fontconfig's, to tell a bold face from one to embolden */
	unsigned long used; /* fallback faces: font.tick when last found */
	bool mono; /* fallback faces: fontconfig says monospaced or dual */
};

struct entry {
	uint32_t key; /* cp << 3 | wide << 2 | style */
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
	long long sort_retry; /* ms, after a failure */
	FcPattern *sort_pat;
	FcFontSet *sorted;
	FcCharSet *cover;
	/* sorted's indices, the monospaced fonts first, nmono of them; and the
	 * fonts that will not load, so that they are not tried at each glyph */
	int *order, nmono;
	bool *dead;
	unsigned long tick;
	double size_pt, size_px;
	int slant; /* regular's, for the fallback faces */
	int cw, ch, ascent;
	struct entry *cache;
	size_t cache_cap, cache_used;
	uint32_t hash_seed; /* random: output cannot pick keys that collide */
	size_t cache_bytes; /* of the bitmaps in it */
};

/* 1 when loaded, 0 out of memory, -1 for a face that will not load: not
 * scalable, as Noto Color Emoji, or unreadable */
static int load_face(struct font *f, FcPattern *match, struct face *out)
{
	FcChar8 *file;
	int index = 0;
	FcBool emb = FcFalse;
	FcMatrix *fcm;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch)
		return -1;
	FcPatternGetInteger(match, FC_INDEX, 0, &index);
	FcPatternGetBool(match, FC_EMBOLDEN, 0, &emb);
	out->weight = FC_WEIGHT_REGULAR;
	FcPatternGetDouble(match, FC_WEIGHT, 0, &out->weight);
	FT_Error err = FT_New_Face(f->lib, (const char *)file, index, &out->ft);
	if (err)
		return err == FT_Err_Out_Of_Memory ? 0 : -1;
	if (!FT_IS_SCALABLE(out->ft)) {
		FT_Done_Face(out->ft);
		out->ft = NULL;
		return -1;
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
		return 0;
	}
	out->embolden = emb;
	out->index = index;
	return 1;
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
	/* the size in points too: with the pixels alone fontconfig takes it from
	 * them at 75 dpi, and a font with optical sizes gives the wrong one */
	FcPatternAddDouble(f->base, FC_SIZE, pt);

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
		bool ok = m && load_face(f, m, &f->style[i]) > 0;
		if (ok && i == 0) {
			/* fontconfig gives some font for any name: a family it does not
			 * know comes out proportional, and the grid does not line up */
			static bool told; /* once: a zoom opens the font again */
			int spacing = FC_PROPORTIONAL;
			FcPatternGetInteger(m, FC_SPACING, 0, &spacing);
			/* FreeType too: Noto Sans Mono sets no spacing */
			if (spacing < FC_DUAL && !FT_IS_FIXED_WIDTH(f->style[0].ft) && !told) {
				told = true;
				FcChar8 *fam = NULL;
				FcPatternGetString(m, FC_FAMILY, 0, &fam);
				fprintf(stderr, "mwt: '%s' gave %s, which is not monospaced\n", spec,
					fam ? (const char *)fam : "a font");
			}
			rweight = f->style[0].weight;
			FcPatternGetDouble(m, FC_WIDTH, 0, &rwidth);
			FcPatternGetInteger(m, FC_SLANT, 0, &rslant);
			f->slant = rslant;
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
	if (gi && !FT_Load_Glyph(r, gi, LOAD_FLAGS))
		f->cw = (int)lround(r->glyph->advance.x / 64.0);
	if (f->cw < 1)
		f->cw = 1;
	if (f->ch < 1 || f->ascent < 1) { /* FreeType refused the size */
		fprintf(stderr, "mwt: font size %g is out of range\n", pt);
		goto fail;
	}

	if (getrandom(&f->hash_seed, sizeof f->hash_seed, GRND_NONBLOCK) != sizeof f->hash_seed)
		f->hash_seed = (uint32_t)time(NULL) * 2654435761u ^ (uint32_t)getpid() ^
			       (uint32_t)(uintptr_t)f;
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
	free(f->order);
	free(f->dead);
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
static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static bool is_mono(FcPattern *p)
{
	int spacing = FC_PROPORTIONAL;
	FcPatternGetInteger(p, FC_SPACING, 0, &spacing);
	return spacing >= FC_DUAL;
}

static bool sorted_fonts(struct font *f)
{
	if (f->sorted_tried && (f->sorted && f->cover && f->order && f->dead))
		return true;
	if (f->sorted_tried && now_ms() < f->sort_retry)
		return false;
	/* first, or again after a failure: what that left goes */
	if (f->sorted)
		FcFontSetDestroy(f->sorted);
	if (f->cover)
		FcCharSetDestroy(f->cover);
	if (f->sort_pat)
		FcPatternDestroy(f->sort_pat);
	free(f->order);
	free(f->dead);
	f->sorted = NULL;
	f->cover = NULL;
	f->sort_pat = NULL;
	f->order = NULL;
	f->dead = NULL;
	f->sorted_tried = true;
	f->sort_retry = now_ms() + SORT_RETRY_MS;
	f->sort_pat = FcPatternDuplicate(f->base);
	if (!f->sort_pat) /* FcPatternAdd* and FcConfigSubstitute do not take NULL */
		return false;
	/* The fallback faces as regular came out, by weight and slant: a bold or
	 * light spec gets bold or light ones. Not by the style name, which says
	 * nothing in another family and outranks the weight. */
	FcPatternDel(f->sort_pat, FC_STYLE);
	FcPatternDel(f->sort_pat, FC_WEIGHT);
	FcPatternAddDouble(f->sort_pat, FC_WEIGHT, f->style[STYLE_REGULAR].weight);
	FcPatternDel(f->sort_pat, FC_SLANT);
	FcPatternAddInteger(f->sort_pat, FC_SLANT, f->slant);
	FcPatternAddBool(f->sort_pat, FC_SCALABLE, FcTrue);
	FcConfigSubstitute(NULL, f->sort_pat, FcMatchPattern);
	FcDefaultSubstitute(f->sort_pat);
	FcResult res;
	/* Not trimmed: a trimmed list leaves out a font that adds nothing to those
	 * before it, and so D2Coding behind NanumGothic, the monospaced face behind
	 * the proportional one find_fallback should pass over. */
	f->sorted = FcFontSort(NULL, f->sort_pat, FcFalse, &f->cover, &res);
	if (!f->sorted || !f->cover)
		return false;
	int n = f->sorted->nfont;
	f->order = malloc((n ? n : 1) * sizeof *f->order);
	f->dead = calloc(n ? n : 1, sizeof *f->dead);
	if (!f->order || !f->dead)
		return false;
	/* once: walking the whole list for the few monospaced fonts cost 5 µs a
	 * glyph with 300 fonts */
	f->nmono = 0;
	for (int k = 0; k < n; k++)
		if (is_mono(f->sorted->fonts[k]))
			f->order[f->nmono++] = k;
	for (int k = 0, j = f->nmono; k < n; k++)
		if (!is_mono(f->sorted->fonts[k]))
			f->order[j++] = k;
	return true;
}

/* How close the style face i is to style, for one that does not fit: what they
 * share first, then the less it adds. */
static int style_near(int i, int style)
{
	return 4 * __builtin_popcount((unsigned)(i & style)) - __builtin_popcount((unsigned)(i & ~style));
}

static struct face *found(struct font *f, struct face *fc)
{
	fc->used = ++f->tick;
	return fc;
}

/* find or load a face that has cp; NULL if none on the system. Monospaced faces
 * first, open or not, by fontconfig's FC_SPACING: their glyphs are drawn for a
 * grid and need less fitting (6.6 of the review of 2026-11-18). The style faces
 * that fit were tried before; one that does not comes last, from the font's
 * own file: U+27BF, only in DejaVu's bold file, would be bold in regular. */
static struct face *find_fallback(struct font *f, uint32_t cp, int style)
{
	struct face *other = NULL, *prop = NULL;
	for (int i = 0; i < f->nfallback; i++)
		if (FT_Get_Char_Index(f->fallback[i].ft, cp)) {
			if (f->fallback[i].mono)
				return found(f, &f->fallback[i]);
			if (!prop)
				prop = &f->fallback[i];
		}
	if (!sorted_fonts(f) || !FcCharSetHasChar(f->cover, cp))
		return prop ? found(f, prop) : NULL;
	/* the monospaced fonts, then the others; an open proportional face
	 * before loading another */
	for (int j = 0; j < (prop ? f->nmono : f->sorted->nfont); j++) {
		int k = f->order[j];
		bool mono = j < f->nmono;
		FcCharSet *cs;
		if (f->dead[k] ||
		    FcPatternGetCharSet(f->sorted->fonts[k], FC_CHARSET, 0, &cs) != FcResultMatch ||
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
		int ok = load_face(f, m, &nf);
		FcPatternDestroy(m);
		if (ok <= 0) { /* the next may do */
			f->dead[k] = ok < 0;
			continue;
		}
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
		nf.mono = mono;
		f->fallback[slot] = nf;
		return found(f, &f->fallback[slot]);
	}
	return prop ? found(f, prop) : other;
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

static FT_GlyphSlot load(struct face *fc, FT_UInt gi, bool embolden)
{
	if (FT_Load_Glyph(fc->ft, gi, LOAD_FLAGS))
		return NULL;
	FT_GlyphSlot s = fc->ft->glyph;
	if (embolden)
		FT_GlyphSlot_Embolden(s);
	if (FT_Render_Glyph(s, FT_RENDER_MODE_NORMAL))
		return NULL;
	return s;
}

static void set_size(struct face *fc, double px)
{
	FT_Set_Char_Size(fc->ft, 0, (FT_F26Dot6)fmax(1, lround(px * 64)), 72, 72);
}

/* A glyph of a fallback face in its cells, w wide (6.2 of the review of
 * 2026-11-18): a proportional font draws ① or a hanja wider than the grid, and
 * taller than its row. One too large is drawn smaller and centred, across, and
 * down only where it was too tall: a letter a little too wide keeps its
 * baseline, in line with the others of its word. One that fits is centred by
 * its advance, as a monospaced font would place it. Either is moved into the
 * cells where it sticks out. NULL for none. */
static FT_GlyphSlot fit(struct font *f, struct face *fc, FT_UInt gi, bool embolden, int w,
			FT_GlyphSlot s, int *left, int *top)
{
	int bw = (int)s->bitmap.width, bh = (int)s->bitmap.rows;
	if (bw > w || bh > f->ch) {
		bool tall = bh > f->ch;
		/* hinting may round the smaller size up a pixel: a few tries */
		double px = f->size_px;
		for (int i = 0; i < 4 && s && (bw > w || bh > f->ch); i++) {
			px *= fmin((double)w / bw, (double)f->ch / bh) * (i ? 0.97 : 1);
			set_size(fc, px);
			s = load(fc, gi, embolden);
			if (s) {
				bw = (int)s->bitmap.width;
				bh = (int)s->bitmap.rows;
			}
		}
		/* the bitmap stays in the slot: the face is shared */
		set_size(fc, f->size_px);
		/* U+FDFD, twelve cells wide, below a pixel high in one: nothing
		 * rather than over the next cells */
		if (!s || bw > w || bh > f->ch)
			return NULL;
		*left = (w - bw) / 2;
		*top = tall ? f->ascent - (f->ch - bh) / 2 : s->bitmap_top;
	} else {
		int adv = (int)lround(s->advance.x / 64.0);
		*left = s->bitmap_left + (adv > 0 ? (w - adv) / 2 : 0);
		*top = s->bitmap_top;
	}
	*left = *left < 0 ? 0 : *left > w - bw ? w - bw : *left;
	if (*top > f->ascent)
		*top = f->ascent;
	if (*top - bh < f->ascent - f->ch)
		*top = f->ascent - f->ch + bh;
	return s;
}

/* False when g is empty for lack of memory, and may not be the next time: the
 * bitmap, or the list of the system's fonts to find a face in. */
static bool render(struct font *f, uint32_t cp, int style, bool wide, struct glyph *g)
{
	memset(g, 0, sizeof *g);
	bool embolden;
	struct face *fc = face_for(f, cp, style, &embolden);
	FT_UInt gi = fc ? FT_Get_Char_Index(fc->ft, cp) : 0;
	if (!gi)
		return !f->sorted_tried || (f->sorted && f->cover && f->order && f->dead);
	FT_GlyphSlot s = load(fc, gi, embolden);
	if (!s || !s->bitmap.width || !s->bitmap.rows)
		return true;
	int left = s->bitmap_left, top = s->bitmap_top;
	if (fc >= f->fallback && fc < f->fallback + MAX_FALLBACK) {
		s = fit(f, fc, gi, embolden, (wide ? 2 : 1) * f->cw, s, &left, &top);
		if (!s || !s->bitmap.width || !s->bitmap.rows)
			return true;
	}
	FT_Bitmap *b = &s->bitmap;
	g->w = b->width;
	g->h = b->rows;
	g->left = left;
	g->top = top;
	g->alpha = malloc((size_t)g->w * g->h);
	if (!g->alpha) {
		g->w = g->h = 0;
		return false;
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
	return true;
}

/* murmur3's finalizer: every bit of the key moves every bit of the result. */
static uint32_t mix(uint32_t h)
{
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	return h ^ (h >> 16);
}

/* The key mixed with a seed output cannot know, its top bits. A fixed multiplier
 * let about 200 KB of chosen code points pile 64k glyphs into one run (631 ms a
 * frame), by its low bits or by its top ones; a random multiplier alone still
 * ran code points in a row into long runs for some. cap is a power of two. */
static struct entry *slot(struct entry *tab, size_t cap, uint32_t key, uint32_t seed)
{
	size_t i = mix(key ^ seed) >> (32 - __builtin_ctzll(cap));
	while (tab[i].used && tab[i].key != key) {
		SLOT_PROBE();
		i = (i + 1) & (cap - 1);
	}
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

const struct glyph *font_glyph(struct font *f, uint32_t cp, int style, bool wide)
{
	uint32_t key = (cp & 0x1fffff) << 3 | (uint32_t)wide << 2 | (style & 3);
	struct entry *e = slot(f->cache, f->cache_cap, key, f->hash_seed);
	if (e->used)
		return &e->g;
	if (f->cache_used >= GLYPH_CACHE_MAX || f->cache_bytes >= GLYPH_CACHE_BYTES) {
		cache_empty(f);
		e = slot(f->cache, f->cache_cap, key, f->hash_seed);
	}
	if ((f->cache_used + 1) * 10 > f->cache_cap * 7) {
		size_t ncap = f->cache_cap * 2;
		struct entry *n = calloc(ncap, sizeof *n);
		if (n) {
			for (size_t i = 0; i < f->cache_cap; i++)
				if (f->cache[i].used)
					*slot(n, ncap, f->cache[i].key, f->hash_seed) = f->cache[i];
			free(f->cache);
			f->cache = n;
			f->cache_cap = ncap;
			e = slot(f->cache, f->cache_cap, key, f->hash_seed);
		} else if (f->cache_used + 1 >= f->cache_cap) {
			/* no memory to grow, and a full table would leave slot()
			 * without a free place: draw nothing for this one */
			static const struct glyph none;
			return &none;
		}
	}
	if (!render(f, cp, style & 3, wide, &e->g)) {
		/* not kept: the next time may have the memory. The slot was free
		 * and is the end of its run, so leaving it free breaks no run. */
		static const struct glyph none;
		return &none;
	}
	e->used = true;
	e->key = key;
	f->cache_used++;
	f->cache_bytes += (size_t)e->g.w * e->g.h;
	return &e->g;
}
