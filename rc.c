/* rc.c - ~/.config/mwt/mwtrc, in the syntax of mwc's mwmrc:
 *
 *   Mwt*foreground:            #e5e5e5
 *   Mwt*background:            #000040
 *   Mwt*cursorColor:           #e5e5e5     default: the foreground
 *   Mwt*selectionForeground:   #000040
 *   Mwt*selectionBackground:   #a0a0a0
 *   Mwt*indigo*foreground:     #dcdcdc     only for mwt -a indigo
 *
 * Names and the app_id are compared ignoring case. Lines starting with ! are
 * comments; there are no comments after a value. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "rc.h"

enum { FG, BG, CURSOR, SEL_FG, SEL_BG, N_RES };

static const char *const names[N_RES] = {
	"foreground", "background", "cursorColor", "selectionForeground",
	"selectionBackground",
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
		if (!rc_color(value, &val[lvl][r])) {
			fprintf(stderr, "mwt: %s:%d: %s: not a color: \"%s\" "
				"(#rrggbb, #rgb or rgb:rr/gg/bb)\n", path, no, names[r], value);
			continue;
		}
		set[lvl][r] = true;
	}
	free(line);

	bool cursor_set = false;
	uint32_t *out[N_RES] = { &c->fg, &c->bg, &c->cursor, &c->sel_fg, &c->sel_bg };
	for (int r = 0; r < N_RES; r++) {
		for (int lvl = 0; lvl < 2; lvl++)
			if (set[lvl][r])
				*out[r] = val[lvl][r];
		if (r == CURSOR)
			cursor_set = set[0][r] || set[1][r];
	}
	if (!cursor_set && (set[0][FG] || set[1][FG]))
		c->cursor = c->fg;
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
