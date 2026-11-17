/* tests/font_style.c - a character the regular face has is drawn in every
 * style, also where the bold or italic face lacks it. font.c is built in so
 * that this file can ask the faces what they cover. Needs a font whose style
 * faces cover less than its regular one, as DejaVu Sans Mono does; make fonts
 * runs it. */
#include "../font.c"

#include <stdio.h>

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	struct font *f = font_open(spec, 0, 1);
	if (!f) {
		fprintf(stderr, "font_style: no font for '%s'\n", spec);
		return 1;
	}
	static const char *name[4] = { "regular", "bold", "italic", "bold italic" };
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
	font_close(f);
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
