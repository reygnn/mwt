/* box.h - box drawing and block elements, U+2500 to U+259F, drawn to fill the
 * cell rather than taken from the font: Noto Sans Mono's │ and █ stop 1 to 3 px
 * short, and tmux borders and htop meters came out dashed. No FreeType, no
 * Wayland: font.c asks it first, test_box checks the shapes headless. */
#ifndef BOX_H
#define BOX_H

#include <stdbool.h>
#include <stdint.h>

/* The light stroke for a font whose underline is underline pixels thick (0 or
 * less for none: a tenth of the cell width cw), at least a pixel a scale, as
 * mwt draws its underline. */
int box_thin(double underline, int cw, int scale);

/* Whether cp is drawn here. */
bool box_covers(uint32_t cp);

/* cp's coverage into alpha, w * h bytes row by row, which it clears first. thin
 * is a light stroke in pixels, a heavy one twice that, a double line two thin
 * strokes a thin one apart; bold makes light strokes heavy. Lines run through
 * the middle of the cell to its edges, so neighbours join. */
void box_draw(uint32_t cp, int w, int h, int thin, bool bold, uint8_t *alpha);

#endif
