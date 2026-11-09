/* rc.h - ~/.config/mwt/mwtrc: colors at startup, over those of config.h */
#ifndef MWT_RC_H
#define MWT_RC_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

struct rc_colors {
	uint32_t fg, bg, cursor, sel_fg, sel_bg; /* 0xrrggbb */
};

/* #rgb, #rrggbb or rgb:r/g/b with 1 to 4 hex digits per channel. */
bool rc_color(const char *s, uint32_t *out);

/* Reads Motif-style resource lines from f and changes what they name in c,
 * which holds the defaults. A line for app_id ("Mwt*name*foreground") wins
 * over a global one ("Mwt*foreground"), later lines over earlier ones. The
 * cursor follows the foreground unless a cursorColor line is there. Bad lines
 * are skipped with a message on stderr naming path and line. */
void rc_read(FILE *f, const char *path, const char *app_id, struct rc_colors *c);

/* $XDG_CONFIG_HOME/mwt/mwtrc or ~/.config/mwt/mwtrc, into buf; false if
 * neither variable is set or the path does not fit. */
bool rc_path(char *buf, size_t size);

#endif
