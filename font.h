/* font.h - fontconfig/FreeType glyph loading with a small cache */
#ifndef FONT_H
#define FONT_H

#include <stdint.h>

enum { STYLE_REGULAR, STYLE_BOLD, STYLE_ITALIC, STYLE_BOLD_ITALIC };

struct glyph {
	int w, h;        /* bitmap size, 0 for empty/missing */
	int left, top;   /* bearing relative to pen position and baseline */
	uint8_t *alpha;  /* w*h coverage values */
};

struct font;

/* spec is a fontconfig pattern like "monospace:size=11";
 * size_pt overrides the size in spec when > 0; scale is the buffer scale */
struct font *font_open(const char *spec, double size_pt, int scale);
void font_close(struct font *f);
const struct glyph *font_glyph(struct font *f, uint32_t cp, int style);
void font_metrics(const struct font *f, int *cw, int *ch, int *ascent);
double font_size_pt(const struct font *f);

#endif
