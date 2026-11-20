/* rc.h - ~/.config/mwt/mwtrc: colors at startup, over those of config.h, and
 * the minimum contrast between text and its background */
#ifndef MWT_RC_H
#define MWT_RC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct rc_colors {
	uint32_t fg, bg, cursor, sel_fg, sel_bg; /* 0xrrggbb */
	uint32_t palette[16];                    /* color0 to color15 */
	float min_contrast;                      /* minimumContrast, <= 1: off */
};

/* #rgb, #rrggbb or rgb:r/g/b with 1 to 4 hex digits per channel. */
bool rc_color(const char *s, uint32_t *out);

/* Reads Motif-style resource lines from f and changes what they name in c,
 * which holds the defaults. A line for app_id ("Mwt*name*foreground") wins
 * over a global one ("Mwt*foreground"), later lines over earlier ones. The
 * cursor follows the foreground unless a cursorColor line is there. Bad lines
 * are skipped with a message on stderr naming path and line. */
void rc_read(FILE *f, const char *path, const char *app_id, struct rc_colors *c);

/* WCAG contrast ratio of two 0xrrggbb colors, 1 to 21. */
double rc_contrast(uint32_t a, uint32_t b);

/* The color to draw text of color fg in on bg: fg itself, or, if its contrast
 * is below c->min_contrast, the theme's foreground or background, whichever
 * stands out more, and only if that is better than fg. fg == bg is left
 * alone: that hides text on purpose. */
uint32_t rc_readable(uint32_t fg, uint32_t bg, const struct rc_colors *c);

/* The cursor's color on a cell drawn fg on bg: cursor, or where it hardly
 * stands out from bg, as on a reverse cell with the cursor in the foreground
 * color, fg, if that does better: the cell reversed, as xterm draws it. */
uint32_t rc_cursor(uint32_t cursor, uint32_t fg, uint32_t bg);

/* $XDG_CONFIG_HOME/mwt/mwtrc or ~/.config/mwt/mwtrc, into buf; false if
 * neither variable is set or the path does not fit. */
bool rc_path(char *buf, size_t size);

#endif
