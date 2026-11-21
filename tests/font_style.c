/* tests/font_style.c - which face each style draws a character from. One the
 * regular face has is drawn in every style, also where the bold or italic face
 * lacks it; one only a style face has comes from a style face that fits before
 * any other font, from one that does not only where no font has it, the same
 * whatever was drawn before; a bold face is not made bolder; a style name in
 * the spec keeps the four faces. font.c is built in so that this file can ask
 * the faces what they cover. Needs a font whose style faces cover less than
 * its regular one and have some it lacks, as DejaVu Sans Mono does; make fonts
 * runs it. */
#include "../font.c"

#include <stdio.h>

static const char *name[4] = { "regular", "bold", "italic", "bold italic" };
static int fails;

/* Whether the style face i may stand in for style: it adds nothing that was not
 * asked for. Bold Italic takes Bold; Regular takes none of the others. */
static bool style_fits(int i, int style)
{
	return (i & ~style) == 0;
}

#define FAIL(...) do { if (fails++ < 10) printf("FAIL: " __VA_ARGS__); } while (0)

static bool is_style_face(struct font *f, const char *file, int index)
{
	for (int i = 0; i < 4; i++)
		if (!strcmp(f->style[i].path, file) && f->style[i].index == index)
			return true;
	return false;
}

/* a font on the system, not one of the style faces, that has cp: asked of
 * fontconfig's list, not of the sorted one font.c walks */
static bool other_font_has(struct font *f, uint32_t cp)
{
	FcPattern *p = FcPatternCreate();
	FcCharSet *cs = FcCharSetCreate();
	FcObjectSet *os = FcObjectSetBuild(FC_FILE, FC_INDEX, FC_SCALABLE, (char *)NULL);
	FcCharSetAddChar(cs, cp);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddBool(p, FC_SCALABLE, FcTrue);
	FcFontSet *set = FcFontList(NULL, p, os);
	bool found = false;
	for (int k = 0; set && k < set->nfont && !found; k++) {
		FcChar8 *file;
		int index = 0;
		if (FcPatternGetString(set->fonts[k], FC_FILE, 0, &file) != FcResultMatch)
			continue;
		FcPatternGetInteger(set->fonts[k], FC_INDEX, 0, &index);
		found = !is_style_face(f, (const char *)file, index);
	}
	if (set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcCharSetDestroy(cs);
	FcPatternDestroy(p);
	return found;
}

static void check_bold(uint32_t cp, int s, struct face *fc, bool emb);

/* a font on the system of at least or below weight that has cp */
static bool weight_font_has(uint32_t cp, bool bold)
{
	FcPattern *p = FcPatternCreate();
	FcCharSet *cs = FcCharSetCreate();
	FcObjectSet *os = FcObjectSetBuild(FC_WEIGHT, (char *)NULL);
	FcCharSetAddChar(cs, cp);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcPatternAddBool(p, FC_SCALABLE, FcTrue);
	FcFontSet *set = FcFontList(NULL, p, os);
	bool found = false;
	for (int k = 0; set && k < set->nfont && !found; k++) {
		double w;
		if (FcPatternGetDouble(set->fonts[k], FC_WEIGHT, 0, &w) == FcResultMatch)
			found = bold ? w >= FC_WEIGHT_DEMIBOLD : w < FC_WEIGHT_DEMIBOLD;
	}
	if (set)
		FcFontSetDestroy(set);
	FcObjectSetDestroy(os);
	FcCharSetDestroy(cs);
	FcPatternDestroy(p);
	return found;
}

/* The fallback faces in regular text are as bold as regular: a bold face, or
 * one made bold, with a bold regular; a regular one not made bold otherwise,
 * where the system has one for the character. The first 200 characters
 * none of the style faces has. */
static void fallback_weight(const char *spec, struct font *f)
{
	bool bold = f->style[STYLE_REGULAR].weight >= FC_WEIGHT_DEMIBOLD;
	int n = 0;
	for (uint32_t cp = 0x21; cp < 0x10000 && n < 200; cp++) {
		bool style = false;
		for (int i = 0; i < 4; i++)
			style |= FT_Get_Char_Index(f->style[i].ft, cp) != 0;
		if (style)
			continue;
		bool emb;
		struct face *fc = face_for(f, cp, STYLE_REGULAR, &emb);
		if (!fc)
			continue;
		n++;
		bool drawn_bold = emb || fc->weight >= FC_WEIGHT_DEMIBOLD;
		if (bold && !drawn_bold)
			FAIL("'%s': U+%04X from the regular %s, not made bold\n", spec,
			     (unsigned)cp, fc->path);
		if (!bold && drawn_bold && weight_font_has(cp, false))
			FAIL("'%s': U+%04X bold from %s, a regular font has it\n", spec,
			     (unsigned)cp, fc->path);
		check_bold(cp, STYLE_REGULAR, fc, emb);
		check_bold(cp, STYLE_BOLD, face_for(f, cp, STYLE_BOLD, &emb), emb);
	}
	if (!n)
		FAIL("'%s': no character from a fallback face, nothing was tested\n", spec);
}

static int style_index(struct font *f, struct face *fc)
{
	return fc >= f->style && fc < f->style + 4 ? (int)(fc - f->style) : -1;
}

/* A face that is bold already is not emboldened. */
static void check_bold(uint32_t cp, int s, struct face *fc, bool emb)
{
	if (fc && emb && !fc->embolden && fc->weight >= FC_WEIGHT_DEMIBOLD)
		FAIL("U+%04X in %s: a bold face (%s) made bolder\n", (unsigned)cp, name[s], fc->path);
}

/* What regular has: drawn in every style, and not made bolder than bold. */
static int from_regular(struct font *f)
{
	int gaps = 0;
	for (uint32_t cp = 0x21; cp < 0x10000; cp++) {
		if (!FT_Get_Char_Index(f->style[STYLE_REGULAR].ft, cp))
			continue;
		if (!font_glyph(f, cp, STYLE_REGULAR, false)->w)
			continue; /* blank in regular too */
		for (int s = 1; s < 4; s++) {
			bool emb = false;
			check_bold(cp, s, face_for(f, cp, s, &emb), emb);
			if (FT_Get_Char_Index(f->style[s].ft, cp))
				continue;
			gaps++;
			if (!font_glyph(f, cp, s, false)->w)
				FAIL("U+%04X is drawn in regular but not in %s\n", (unsigned)cp, name[s]);
		}
	}
	return gaps;
}

/* What regular lacks and a style face has, in each style on a fresh font, and
 * again after the other styles drew it. */
static int style_only(const char *spec, struct font *f)
{
	int tested = 0;
	for (uint32_t cp = 0x21; cp < 0x10000; cp++) {
		if (FT_Get_Char_Index(f->style[STYLE_REGULAR].ft, cp))
			continue;
		bool some = false;
		for (int i = 1; i < 4; i++)
			some |= FT_Get_Char_Index(f->style[i].ft, cp) != 0;
		if (!some)
			continue;
		tested++;
		bool other = other_font_has(f, cp);
		for (int s = 0; s < 4; s++) {
			struct font *a = font_open(spec, 0, 1), *b = font_open(spec, 0, 1);
			if (!a || !b) {
				FAIL("no font for '%s'\n", spec);
				font_close(a);
				font_close(b);
				return tested;
			}
			bool emb = false, embb = false;
			struct face *fc = face_for(a, cp, s, &emb);
			for (int o = 0; o < 4; o++)
				if (o != s)
					face_for(b, cp, o, &embb);
			struct face *fb = face_for(b, cp, s, &embb);
			int i = style_index(a, fc);
			bool fits = false;
			for (int j = 0; j < 4; j++)
				fits |= style_fits(j, s) && FT_Get_Char_Index(a->style[j].ft, cp);
			if (!fc)
				FAIL("U+%04X is not drawn in %s\n", (unsigned)cp, name[s]);
			else if (fits && (i < 0 || !style_fits(i, s)))
				FAIL("U+%04X in %s from %s, though a style face fits\n", (unsigned)cp,
				     name[s], fc->path);
			else if (!fits && i >= 0 && other)
				FAIL("U+%04X in %s from the %s face, another font has it\n",
				     (unsigned)cp, name[s], name[i]);
			if (fc && fb && (strcmp(fc->path, fb->path) || fc->index != fb->index ||
					 emb != embb))
				FAIL("U+%04X in %s from %s, after the other styles from %s\n",
				     (unsigned)cp, name[s], fc->path, fb->path);
			check_bold(cp, s, fc, emb);
			font_close(a);
			font_close(b);
		}
	}
	return tested;
}

/* spec:style=<regular's style, as fontconfig names it> gives the faces the
 * spec alone gives */
static void spec_style(const char *spec, struct font *plain)
{
	int count;
	FcPattern *q = FcFreeTypeQuery((const FcChar8 *)plain->style[STYLE_REGULAR].path,
				       plain->style[STYLE_REGULAR].index, NULL, &count);
	FcChar8 *style;
	if (!q || FcPatternGetString(q, FC_STYLE, 0, &style) != FcResultMatch) {
		FAIL("no style name for %s\n", plain->style[STYLE_REGULAR].path);
		if (q)
			FcPatternDestroy(q);
		return;
	}
	char buf[512];
	snprintf(buf, sizeof buf, "%s:style=%s", spec, (const char *)style);
	FcPatternDestroy(q);
	struct font *f = font_open(buf, 0, 1);
	if (!f) {
		FAIL("no font for '%s'\n", buf);
		return;
	}
	for (int i = 0; i < 4; i++)
		if (strcmp(f->style[i].path, plain->style[i].path) ||
		    f->style[i].index != plain->style[i].index ||
		    f->style[i].embolden != plain->style[i].embolden)
			FAIL("'%s' gives %s from %s, without the style from %s\n", buf, name[i],
			     f->style[i].path, plain->style[i].path);
	font_close(f);
}

/* The other faces keep regular's weight, bold at least bold: with a bold
 * regular italic is not lighter. A face synthesized from regular's file is. */
static void follow_weight(const char *spec, struct font *f)
{
	double r = f->style[STYLE_REGULAR].weight;
	for (int i = 1; i < 4; i++) {
		double want = i & STYLE_BOLD && r < FC_WEIGHT_BOLD ? FC_WEIGHT_BOLD : r;
		if (f->style[i].weight < want && !f->style[i].embolden)
			FAIL("'%s': %s has weight %g, regular %g\n", spec, name[i],
			     f->style[i].weight, r);
	}
}

/* Box drawing fills the cell from box.c, not the face: │ inks every row of the
 * cell, which Noto Sans Mono's left short; at least a pixel a scale thick, and
 * bold is heavier. */
static void box_cells(const char *spec)
{
	for (int scale = 1; scale <= 3; scale++)
		for (double pt = 6; pt <= 20; pt += 1.5) {
			struct font *f = font_open(spec, pt, scale);
			if (!f)
				continue;
			const struct glyph *g = font_glyph(f, 0x2502, STYLE_REGULAR, false);
			bool whole = g->w == f->cw && g->h == f->ch && g->top == f->ascent && !g->left;
			int x = (g->w - f->box_thin) / 2;
			for (int y = 0; whole && y < g->h; y++)
				whole = g->alpha[y * g->w + x] == 255;
			if (!whole)
				FAIL("'%s' at %g pt, scale %d: │ does not fill its %dx%d cell\n", spec, pt,
				     scale, f->cw, f->ch);
			int light = 0, heavy = 0;
			g = font_glyph(f, 0x2500, STYLE_REGULAR, false);
			for (int y = 0; y < g->h; y++)
				light += g->alpha[y * g->w] == 255;
			g = font_glyph(f, 0x2500, STYLE_BOLD, false);
			for (int y = 0; y < g->h; y++)
				heavy += g->alpha[y * g->w] == 255;
			if (light < scale || heavy != 2 * light)
				FAIL("'%s' at %g pt, scale %d: ─ %d px, bold %d px\n", spec, pt, scale,
				     light, heavy);
			font_close(f);
		}
}

/* The cell is as wide as the printable ASCII characters are, loaded as the
 * glyphs are drawn (LOAD_FLAGS). */
static void cell_width(const char *spec)
{
	for (int scale = 1; scale <= 4; scale++)
		for (double pt = 4; pt <= 20; pt += 0.5) {
			struct font *f = font_open(spec, pt, scale);
			if (!f)
				continue;
			FT_Face r = f->style[STYLE_REGULAR].ft;
			int off = 0;
			uint32_t which = 0;
			for (uint32_t cp = 0x21; cp < 0x7f; cp++) {
				FT_UInt gi = FT_Get_Char_Index(r, cp);
				if (gi && !FT_Load_Glyph(r, gi, LOAD_FLAGS) &&
				    f->cw != (int)lround(r->glyph->advance.x / 64.0) && !off++)
					which = cp;
			}
			if (off)
				FAIL("'%s' at %g pt, scale %d: cells %d wide, %d characters not, '%c' %.2f\n",
				     spec, pt, scale, f->cw, off, (char)which,
				     (FT_Load_Glyph(r, FT_Get_Char_Index(r, which), LOAD_FLAGS),
				      r->glyph->advance.x / 64.0));
			font_close(f);
		}
}

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	/* and with a bold regular: no face made bolder that is bold */
	char bold[512];
	snprintf(bold, sizeof bold, "%s:weight=bold", spec);
	const char *specs[2] = { spec, bold };
	int gaps = 0, only = 0;
	for (int n = 0; n < 2; n++) {
		struct font *f = font_open(specs[n], 0, 1);
		if (!f) {
			fprintf(stderr, "font_style: no font for '%s'\n", specs[n]);
			return 1;
		}
		follow_weight(specs[n], f);
		fallback_weight(specs[n], f);
		int g = from_regular(f);
		int o = style_only(specs[n], f);
		if (n == 0) {
			gaps = g;
			only = o;
			spec_style(spec, f);
		}
		font_close(f);
	}
	cell_width(spec);
	box_cells(spec);
	if (!gaps)
		FAIL("no style face of '%s' lacks a character of regular, nothing was tested\n", spec);
	if (!only)
		FAIL("no style face of '%s' has a character regular lacks, nothing was tested\n",
		     spec);
	if (fails) {
		printf("FAIL: %d failures\n", fails);
		return 1;
	}
	printf("font_style: %d characters missing from a style face, all drawn; %d only a "
	       "style face has, each from the right face; also with weight=bold and a style "
	       "in the spec; cells as wide as the ASCII characters; box drawing fills its cells\n", gaps, only);
	return 0;
}
