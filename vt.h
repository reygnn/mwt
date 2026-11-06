/* vt.h - terminal state machine and screen grid, independent of any display */
#ifndef VT_H
#define VT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* colors: 0..255 palette index, or one of the specials, or RGB with flag */
#define COL_DEFAULT_FG 256u
#define COL_DEFAULT_BG 257u
#define COL_RGB        (1u << 24)
#define COL_IS_RGB(c)  ((c) & COL_RGB)

enum {
	ATTR_BOLD      = 1 << 0,
	ATTR_DIM       = 1 << 1,
	ATTR_ITALIC    = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	ATTR_BLINK     = 1 << 4,
	ATTR_REVERSE   = 1 << 5,
	ATTR_INVISIBLE = 1 << 6,
	ATTR_STRIKE    = 1 << 7,
	ATTR_WIDE      = 1 << 8,  /* left half of a double-width glyph */
	ATTR_WDUMMY    = 1 << 9,  /* right half, cp is 0 */
	ATTR_WRAP      = 1 << 10, /* set on last cell: line continues on next */
};

struct cell {
	uint32_t cp;
	uint32_t fg, bg;
	uint16_t attr;
};

enum {
	MODE_APPCURSOR  = 1 << 0,  /* DECCKM */
	MODE_APPKEYPAD  = 1 << 1,  /* DECKPAM */
	MODE_ORIGIN     = 1 << 2,  /* DECOM */
	MODE_AUTOWRAP   = 1 << 3,  /* DECAWM */
	MODE_INSERT     = 1 << 4,  /* IRM */
	MODE_HIDECURSOR = 1 << 5,  /* !DECTCEM */
	MODE_ALTSCREEN  = 1 << 6,
	MODE_BRACKETED  = 1 << 7,  /* 2004 */
	MODE_MOUSE_BTN  = 1 << 8,  /* 1000 */
	MODE_MOUSE_DRAG = 1 << 9,  /* 1002 */
	MODE_MOUSE_ANY  = 1 << 10, /* 1003 */
	MODE_MOUSE_SGR  = 1 << 11, /* 1006 */
	MODE_FOCUS      = 1 << 12, /* 1004 */
	MODE_LNM        = 1 << 13, /* LF also does CR */
};
#define MODE_MOUSE (MODE_MOUSE_BTN | MODE_MOUSE_DRAG | MODE_MOUSE_ANY)

enum cursor_style { CURSOR_BLOCK, CURSOR_UNDERLINE, CURSOR_BAR };

struct vt_cursor {
	int x, y;
	bool wrapnext;
	struct cell tmpl;     /* attributes for newly written cells */
	int charset[2];       /* G0, G1: 0 = ASCII, 1 = DEC special graphics */
	int gl;               /* which of G0/G1 is active */
	bool origin;
};

struct vt_callbacks {
	void (*write)(void *ud, const char *buf, size_t len); /* reply to pty */
	void (*title)(void *ud, const char *title);
	void (*bell)(void *ud);
	void *ud;
};

#define VT_MAX_PARAMS 24
#define VT_OSC_MAX    1024

struct vt {
	int cols, rows;
	struct cell **line;       /* current screen (points to pri or alt) */
	struct cell **pri, **alt;
	bool *dirty;
	bool *tabs;

	struct vt_cursor c, saved_pri, saved_alt;
	int top, bot;             /* scroll region, inclusive */
	unsigned mode;
	enum cursor_style cursor_style;
	uint32_t lastcp;          /* for REP */

	/* scrollback (main screen only) */
	struct cell **hist;
	int *hist_len;
	int hist_cap, hist_count, hist_head; /* head: index of next slot */
	int view;                 /* lines scrolled back, 0 = live */
	uint64_t scrolled;        /* total lines pushed into history */

	/* parser */
	int state;
	uint32_t utf8_cp;
	int utf8_need;
	int params[VT_MAX_PARAMS];
	bool subparam[VT_MAX_PARAMS]; /* param was preceded by ':' */
	int nparams;
	bool param_started;
	char priv;                /* '?', '>', '<', '=' or 0 */
	char inter[2];
	int ninter;
	char osc[VT_OSC_MAX];
	int osc_len;

	struct vt_callbacks cb;
};

struct vt *vt_new(int cols, int rows, int histsize, const struct vt_callbacks *cb);
void vt_free(struct vt *vt);
void vt_write(struct vt *vt, const char *buf, size_t len);
void vt_resize(struct vt *vt, int cols, int rows);
void vt_reset(struct vt *vt);

/* line as shown at screen row r with the current view offset;
 * *len receives the number of valid cells (may be < cols for history) */
const struct cell *vt_view_line(const struct vt *vt, int r, int *len);
/* absolute line addressing for selections: abs = row + scrolled - view */
const struct cell *vt_abs_line(const struct vt *vt, int64_t abs, int *len);
int64_t vt_abs_first(const struct vt *vt); /* oldest abs line in history */
void vt_scroll_view(struct vt *vt, int delta); /* +up into history */
void vt_mark_all(struct vt *vt);

#endif
