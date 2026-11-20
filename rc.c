/* rc.c - ~/.config/mwt/mwtrc, in the syntax of mwc's mwmrc:
 *
 *   Mwt*foreground:            #e5e5e5
 *   Mwt*background:            #000040
 *   Mwt*cursorColor:           #e5e5e5     default: the foreground
 *   Mwt*selectionForeground:   #000040
 *   Mwt*selectionBackground:   #a0a0a0
 *   Mwt*color4:                #6c8fb8     the 16 palette colors, as in xterm
 *   Mwt*indigo*foreground:     #dcdcdc     only for mwt -a indigo
 *   Mwt*minimumContrast:       3           text below that contrast to its
 *                                          background is drawn in the
 *                                          foreground or background; 0: off
 *
 * Names and the app_id are compared ignoring case. Lines starting with ! are
 * comments; there are no comments after a value. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rc.h"

enum { FG, BG, CURSOR, SEL_FG, SEL_BG, COLOR0, MIN_CONTRAST = COLOR0 + 16, N_RES };

static const char *const names[N_RES] = {
	"foreground", "background", "cursorColor", "selectionForeground",
	"selectionBackground",
	"color0", "color1", "color2", "color3", "color4", "color5", "color6",
	"color7", "color8", "color9", "color10", "color11", "color12", "color13",
	"color14", "color15", "minimumContrast",
};

static char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1]))
		*--e = '\0';
	return s;
}

static bool hex_digits(const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (!isxdigit((unsigned char)s[i]))
			return false;
	return true;
}

/* n hex digits as one 8-bit channel, scaled as XParseColor does for rgb:. */
static uint32_t channel(const char *s, size_t n)
{
	char tmp[5] = { 0 };
	memcpy(tmp, s, n);
	unsigned long v = strtoul(tmp, NULL, 16), max = (1ul << (4 * n)) - 1;
	return (uint32_t)((v * 255 + max / 2) / max);
}

bool rc_color(const char *s, uint32_t *out)
{
	size_t len = strlen(s);
	if (s[0] == '#' && (len == 4 || len == 7) && hex_digits(s + 1, len - 1)) {
		size_t n = (len - 1) / 3;
		*out = channel(s + 1, n) << 16 | channel(s + 1 + n, n) << 8 |
		       channel(s + 1 + 2 * n, n);
		return true;
	}
	if (strncasecmp(s, "rgb:", 4))
		return false;
	s += 4;
	uint32_t v = 0;
	for (int i = 0; i < 3; i++) {
		size_t n = strspn(s, "0123456789abcdefABCDEF");
		if (n < 1 || n > 4 || s[n] != (i < 2 ? '/' : '\0'))
			return false;
		v = v << 8 | channel(s, n);
		s += n + 1;
	}
	*out = v;
	return true;
}

void rc_read(FILE *f, const char *path, const char *app_id, struct rc_colors *c)
{
	/* [0] from global lines, [1] from lines for app_id */
	uint32_t val[2][N_RES];
	float num[2]; /* minimumContrast */
	bool set[2][N_RES] = { { false } };
	char *line = NULL;
	size_t cap = 0;
	int no = 0;
	while (getline(&line, &cap, f) != -1) {
		no++;
		char *s = trim(line);
		if (!*s || *s == '!')
			continue;
		char *colon = strchr(s, ':');
		if (!colon) {
			fprintf(stderr, "mwt: %s:%d: expected resource: value\n", path, no);
			continue;
		}
		*colon = '\0';
		char *key = trim(s), *value = trim(colon + 1);
		/* "Mwt*name*resource": '.' and '*' both separate, and the app_id
		 * between them may have dots of its own. */
		size_t prefix = strcspn(key, "*.");
		char *res = key + strlen(key);
		while (res > key && res[-1] != '*' && res[-1] != '.')
			res--;
		if (prefix != 3 || strncasecmp(key, "mwt", 3) || !key[prefix] || !*res) {
			fprintf(stderr, "mwt: %s:%d: expected Mwt*resource or "
				"Mwt*app-id*resource\n", path, no);
			continue;
		}
		int lvl = 0;
		if (res - key > (ptrdiff_t)prefix + 1) {
			char *name = key + prefix + 1;
			res[-1] = '\0';
			if (!app_id || strcasecmp(name, app_id))
				continue; /* for another window */
			lvl = 1;
		}
		int r = 0;
		while (r < N_RES && strcasecmp(res, names[r]))
			r++;
		if (r == N_RES) {
			fprintf(stderr, "mwt: %s:%d: unknown resource %s\n", path, no, res);
			continue;
		}
		if (r == MIN_CONTRAST) {
			char *end;
			double v = strtod(value, &end);
			if (end == value || *end || !(v >= 0 && v <= 21)) {
				fprintf(stderr, "mwt: %s:%d: minimumContrast: not a number "
					"from 0 to 21: \"%s\"\n", path, no, value);
				continue;
			}
			num[lvl] = (float)v;
		} else if (!rc_color(value, &val[lvl][r])) {
			fprintf(stderr, "mwt: %s:%d: %s: not a color: \"%s\" "
				"(#rrggbb, #rgb or rgb:rr/gg/bb)\n", path, no, names[r], value);
			continue;
		}
		set[lvl][r] = true;
	}
	free(line);

	bool cursor_set = false;
	uint32_t *out[N_RES] = { &c->fg, &c->bg, &c->cursor, &c->sel_fg, &c->sel_bg };
	for (int i = 0; i < 16; i++)
		out[COLOR0 + i] = &c->palette[i];
	for (int lvl = 0; lvl < 2; lvl++)
		if (set[lvl][MIN_CONTRAST])
			c->min_contrast = num[lvl];
	for (int r = 0; r < MIN_CONTRAST; r++) {
		for (int lvl = 0; lvl < 2; lvl++)
			if (set[lvl][r])
				*out[r] = val[lvl][r];
		if (r == CURSOR)
			cursor_set = set[0][r] || set[1][r];
	}
	if (!cursor_set && (set[0][FG] || set[1][FG]))
		c->cursor = c->fg;
}

/* One 8-bit sRGB channel, linearized; a table, as truecolor text can change
 * colors at every cell. */
static double lin(uint32_t v)
{
	static double table[256];
	static bool built;
	if (!built) {
		for (int i = 0; i < 256; i++) {
			double x = i / 255.0;
			table[i] = x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
		}
		built = true;
	}
	return table[v & 0xff];
}

static double luminance(uint32_t c)
{
	return 0.2126 * lin(c >> 16 & 0xff) + 0.7152 * lin(c >> 8 & 0xff) +
	       0.0722 * lin(c & 0xff);
}

double rc_contrast(uint32_t a, uint32_t b)
{
	double la = luminance(a), lb = luminance(b);
	return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

uint32_t rc_readable(uint32_t fg, uint32_t bg, const struct rc_colors *c)
{
	if (c->min_contrast <= 1 || fg == bg)
		return fg;
	double have = rc_contrast(fg, bg);
	if (have >= c->min_contrast)
		return fg;
	double with_fg = rc_contrast(c->fg, bg), with_bg = rc_contrast(c->bg, bg);
	uint32_t best = with_fg >= with_bg ? c->fg : c->bg;
	return fmax(with_fg, with_bg) > have ? best : fg;
}

/* Below this two colors read as one: white on #e5e5e5 is 1.27. */
#define CURSOR_MIN_CONTRAST 1.5

uint32_t rc_cursor(uint32_t cursor, uint32_t fg, uint32_t bg)
{
	double have = rc_contrast(cursor, bg);
	if (have >= CURSOR_MIN_CONTRAST)
		return cursor;
	return rc_contrast(fg, bg) > have ? fg : cursor;
}

bool rc_path(char *buf, size_t size)
{
	const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
	int n;
	if (xdg && *xdg)
		n = snprintf(buf, size, "%s/mwt/mwtrc", xdg);
	else if (home && *home)
		n = snprintf(buf, size, "%s/.config/mwt/mwtrc", home);
	else
		return false;
	return n > 0 && (size_t)n < size;
}
