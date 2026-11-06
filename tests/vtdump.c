/* vtdump.c - feeds stdin to the VT core and prints its state as JSON, for
 * tests/terminfo.py. Usage: vtdump cols rows < bytes */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>

static char reply[4096];
static size_t nreply;
static int bells;

static void on_write(void *ud, const char *b, size_t n)
{
	(void)ud;
	for (size_t i = 0; i < n && nreply < sizeof reply; i++)
		reply[nreply++] = b[i];
}

static void on_bell(void *ud)
{
	(void)ud;
	bells++;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	setlocale(LC_CTYPE, "C.UTF-8");
	struct vt_callbacks cb = { .write = on_write, .bell = on_bell };
	struct vt *vt = vt_new(atoi(argv[1]), atoi(argv[2]), 100, &cb);
	if (!vt)
		return 1;
	static char buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, stdin)) > 0)
		vt_write(vt, buf, n);

	static const struct { unsigned bit; const char *name; } modes[] = {
		{ MODE_APPCURSOR, "appcursor" }, { MODE_APPKEYPAD, "appkeypad" },
		{ MODE_ORIGIN, "origin" }, { MODE_AUTOWRAP, "autowrap" },
		{ MODE_INSERT, "insert" }, { MODE_HIDECURSOR, "hidecursor" },
		{ MODE_ALTSCREEN, "altscreen" }, { MODE_BRACKETED, "bracketed" },
		{ MODE_MOUSE_BTN, "mousebtn" }, { MODE_MOUSE_DRAG, "mousedrag" },
		{ MODE_MOUSE_ANY, "mouseany" }, { MODE_MOUSE_SGR, "mousesgr" },
		{ MODE_FOCUS, "focus" }, { MODE_LNM, "lnm" },
	};
	static const char *const styles[] = { "block", "underline", "bar" };

	printf("{\"cx\":%d,\"cy\":%d,\"wrapnext\":%s,\"top\":%d,\"bot\":%d,\"hist\":%d,\"bells\":%d,",
	       vt->c.x, vt->c.y, vt->c.wrapnext ? "true" : "false", vt->top, vt->bot,
	       vt->hist_count, bells);
	printf("\"style\":\"%s\",\"modes\":[", styles[vt->cursor_style]);
	const char *sep = "";
	for (size_t i = 0; i < sizeof modes / sizeof *modes; i++)
		if (vt->mode & modes[i].bit) {
			printf("%s\"%s\"", sep, modes[i].name);
			sep = ",";
		}
	printf("],\"reply\":\"");
	for (size_t i = 0; i < nreply; i++)
		printf("%02x", (unsigned char)reply[i]);
	printf("\",\"attrbits\":{\"bold\":%d,\"dim\":%d,\"italic\":%d,\"underline\":%d,\"blink\":%d,"
	       "\"reverse\":%d,\"invisible\":%d,\"strike\":%d,\"wide\":%d,\"wdummy\":%d},",
	       ATTR_BOLD, ATTR_DIM, ATTR_ITALIC, ATTR_UNDERLINE, ATTR_BLINK,
	       ATTR_REVERSE, ATTR_INVISIBLE, ATTR_STRIKE, ATTR_WIDE, ATTR_WDUMMY);
	printf("\"defaultfg\":%u,\"defaultbg\":%u,\"rgbflag\":%u,\"cells\":[",
	       COL_DEFAULT_FG, COL_DEFAULT_BG, COL_RGB);
	for (int r = 0; r < vt->rows; r++) {
		printf("%s[", r ? "," : "");
		for (int x = 0; x < vt->cols; x++) {
			const struct cell *c = &vt->line[r][x];
			printf("%s[%u,%u,%u,%u]", x ? "," : "", c->cp, c->fg, c->bg, c->attr);
		}
		printf("]");
	}
	printf("]}\n");
	vt_free(vt);
	return 0;
}
