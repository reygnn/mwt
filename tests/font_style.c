/* tests/font_style.c - a character the regular face has is drawn in every
 * style, also where the bold or italic face lacks it; one only a style face has
 * is taken from it only where nothing else fits; a style name in the spec keeps
 * the four faces. font.c is built in so that this file can ask the faces what
 * they cover. Needs a font whose style faces cover less than its regular one
 * and have some it lacks, as DejaVu Sans Mono does; make fonts runs it. */
#include "../font.c"

#include <stdio.h>

static const char *name[4] = { "regular", "bold", "italic", "bold italic" };

/* a font of the system's list, not one of the style faces, that has cp */
static bool other_font_has(struct font *f, uint32_t cp)
{
	for (int k = 0; k < f->sorted->nfont; k++) {
		FcCharSet *cs;
		FcChar8 *file;
		int index = 0;
		if (FcPatternGetCharSet(f->sorted->fonts[k], FC_CHARSET, 0, &cs) != FcResultMatch ||
		    !FcCharSetHasChar(cs, cp) ||
		    FcPatternGetString(f->sorted->fonts[k], FC_FILE, 0, &file) != FcResultMatch)
			continue;
		FcPatternGetInteger(f->sorted->fonts[k], FC_INDEX, 0, &index);
		bool style = false;
		for (int i = 0; i < 4; i++)
			style |= !strcmp(f->style[i].path, (const char *)file) && f->style[i].index == index;
		if (!style)
			return true;
	}
	return false;
}

/* What regular lacks and a style face has: a style face only where it adds
 * nothing not asked for, or where no other font has it; a bold face not made
 * bolder. Returns the number of failures, *tested what was looked at. */
static int style_only(struct font *f, int *tested)
{
	int fails = 0;
	for (uint32_t cp = 0x21; cp < 0x10000; cp++) {
		if (FT_Get_Char_Index(f->style[STYLE_REGULAR].ft, cp))
			continue;
		bool some = false;
		for (int i = 1; i < 4; i++)
			some |= FT_Get_Char_Index(f->style[i].ft, cp) != 0;
		if (!some)
			continue;
		++*tested;
		for (int s = 0; s < 4; s++) {
			bool emb = false;
			struct face *fc = face_for(f, cp, s, &emb);
			int i = fc ? (int)(fc - f->style) : -1;
			if (i < 0 || i > 3)
				continue; /* a fallback face */
			if (!style_fits(i, s) && other_font_has(f, cp) && fails++ < 5)
				printf("FAIL: U+%04X in %s comes from the %s face, another font has it\n",
				       (unsigned)cp, name[s], name[i]);
			if ((i & STYLE_BOLD) && !fc->embolden && emb && fails++ < 5)
				printf("FAIL: U+%04X in %s: the %s face made bolder\n",
				       (unsigned)cp, name[s], name[i]);
		}
	}
	return fails;
}

/* name:style=<regular's style> must give the faces name alone gives */
static int spec_style(const char *spec, struct font *plain)
{
	char buf[512];
	snprintf(buf, sizeof buf, "%s:style=%s", spec, plain->style[STYLE_REGULAR].ft->style_name);
	struct font *f = font_open(buf, 0, 1);
	if (!f) {
		printf("FAIL: no font for '%s'\n", buf);
		return 1;
	}
	int fails = 0;
	for (int i = 0; i < 4; i++)
		if (strcmp(f->style[i].path, plain->style[i].path) ||
		    f->style[i].embolden != plain->style[i].embolden) {
			printf("FAIL: '%s' gives %s from %s, without the style %s\n", buf, name[i],
			       f->style[i].path, plain->style[i].path);
			fails++;
		}
	font_close(f);
	return fails;
}

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	struct font *f = font_open(spec, 0, 1);
	if (!f) {
		fprintf(stderr, "font_style: no font for '%s'\n", spec);
		return 1;
	}
	int gaps = 0, fails = 0;
	for (uint32_t cp = 0x21; cp < 0x10000; cp++) {
		if (!FT_Get_Char_Index(f->style[STYLE_REGULAR].ft, cp))
			continue;
		if (!font_glyph(f, cp, STYLE_REGULAR)->w)
			continue; /* blank in regular too */
		for (int s = 1; s < 4; s++) {
			if (FT_Get_Char_Index(f->style[s].ft, cp))
				continue;
			gaps++;
			if (!font_glyph(f, cp, s)->w) {
				if (fails++ < 5)
					printf("FAIL: U+%04X is drawn in regular but not in %s\n",
					       (unsigned)cp, name[s]);
			}
		}
	}
	int only = 0, ofails = 0, sfails = 0;
	if (!sorted_fonts(f)) {
		printf("FAIL: no list of the system's fonts\n");
		ofails++;
	} else {
		ofails = style_only(f, &only);
	}
	sfails = spec_style(spec, f);
	font_close(f);
	if (!only) {
		printf("FAIL: no style face of '%s' has a character regular lacks, "
		       "nothing was tested\n", spec);
		return 1;
	}
	if (ofails || sfails) {
		printf("FAIL: %d failures over %d characters only a style face has, %d faces "
		       "wrong with a style in the spec\n", ofails, only, sfails);
		return 1;
	}
	printf("font_style: %d characters only a style face has, each from the right face; "
	       "a style in the spec keeps the faces\n", only);
	if (!gaps) {
		printf("FAIL: no style face of '%s' lacks a character of regular, "
		       "nothing was tested\n", spec);
		return 1;
	}
	if (fails) {
		printf("FAIL: %d of %d characters missing from a style face are not drawn\n",
		       fails, gaps);
		return 1;
	}
	printf("font_style: %d characters missing from a style face, all drawn\n", gaps);
	return 0;
}
