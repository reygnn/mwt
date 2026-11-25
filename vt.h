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
/* only for vt_callbacks.color: the cursor's color, OSC 12 */
#define COL_CURSOR     258u

enum {
	ATTR_BOLD      = 1 << 0,
	ATTR_DIM       = 1 << 1,
	ATTR_ITALIC    = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	/* 1 << 4 free: blink (SGR 5, 6) is not kept, it was never drawn */
	ATTR_REVERSE   = 1 << 5,
	ATTR_INVISIBLE = 1 << 6,
	ATTR_STRIKE    = 1 << 7,
	ATTR_WIDE      = 1 << 8,  /* left half of a double-width glyph */
	ATTR_WDUMMY    = 1 << 9,  /* right half, cp is 0 */
	ATTR_WRAP      = 1 << 10, /* set on last cell: line continues on next */
	ATTR_DECGFX    = 1 << 11, /* cp came from DEC special graphics, for DECRQCRA */
};

struct cell {
	uint32_t cp;
	uint32_t fg, bg;
	uint16_t attr;
};

enum {
	MODE_APPCURSOR  = 1 << 0,  /* DECCKM */
	MODE_APPKEYPAD  = 1 << 1,  /* DECKPAM */
	/* 1 << 2 free: origin mode (DECOM) is the cursor's, c.origin */
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
	/* 0xRRGGBB of a palette index, COL_DEFAULT_FG, COL_DEFAULT_BG or COL_CURSOR,
	 * for OSC 4/10/11/12 queries; without it they get no answer */
	uint32_t (*color)(void *ud, uint32_t c);
	void *ud;
};

/* the grid never gets larger: 8K at the smallest font is about 1500x700 */
#define VT_MAX_COLS 4096
#define VT_MAX_ROWS 2048

#define VT_MAX_PARAMS 24
#define VT_OSC_MAX    1024

struct vt {
	int cols, rows;
	struct cell **line;       /* current screen (points to pri or alt) */
	/* rows of cols + 1 cells: the last is vt.c's own, the cp of it counts the
	 * cells that may differ from a default blank (USED in vt.c) */
	struct cell **pri, **alt;
	bool *dirty;              /* by rows shown: with the view back, screen row + view */
	bool *tabs;

	struct vt_cursor c, saved_pri, saved_alt;
	int top, bot;             /* scroll region, inclusive */
	unsigned mode;
	enum cursor_style cursor_style;
	uint32_t lastcp;          /* for REP */

	/* scrollback (main screen only) */
	struct cell **hist;
	int *hist_len;
	int *hist_room;            /* cells each slot holds; it only grows */
	int hist_cap, hist_count, hist_head; /* head: index of next slot */
	int view;                 /* lines scrolled back, 0 = live */
	uint64_t scrolled;        /* total lines pushed into history */

	/* parser */
	int state;
	uint32_t utf8_cp;
	uint32_t utf8_min;        /* the least value the sequence may encode */
	int utf8_need;
	int params[VT_MAX_PARAMS];
	bool subparam[VT_MAX_PARAMS]; /* param was preceded by ':' */
	int nparams;
	bool params_full;         /* a separator past the last slot: drop the rest */
	bool colon;               /* a ':' among the parameters: sub-parameters */
	char priv;                /* '?', '>', '<', '=' or 0 */
	char inter[2];
	int ninter;
	char osc[VT_OSC_MAX];
	int osc_len;
	bool osc_full;            /* it did not fit: cut there for good */
	bool osc_bel;             /* the OSC ended with BEL, a reply ends the same */

	struct vt_callbacks cb;
	bool allow_checksum;      /* answer DECRQCRA; it lets programs read the screen */
};

/* sizes are clamped to 1..VT_MAX_*; NULL when out of memory */
struct vt *vt_new(int cols, int rows, int histsize, const struct vt_callbacks *cb);
void vt_free(struct vt *vt);
void vt_write(struct vt *vt, const char *buf, size_t len);
/* false when out of memory, and then nothing has changed */
bool vt_resize(struct vt *vt, int cols, int rows);
void vt_reset(struct vt *vt);

/* line as shown at screen row r with the current view offset;
 * *len receives the number of valid cells (may be < cols for history) */
const struct cell *vt_view_line(const struct vt *vt, int r, int *len);
/* absolute line addressing for selections: abs = row + scrolled - view */
const struct cell *vt_abs_line(const struct vt *vt, int64_t abs, int *len);
int64_t vt_abs_first(const struct vt *vt); /* oldest abs line in history */
/* Whether line abs goes on in the next: the cells up to the one with
 * ATTR_WRAP, or 0. Only default blanks may follow that cell, as after the
 * window grew wider: there is no reflow, the flag stays where the line ended. */
int vt_abs_wraps(const struct vt *vt, int64_t abs);
void vt_scroll_view(struct vt *vt, int delta); /* +up into history */
void vt_mark_all(struct vt *vt);

/* Keys that send an escape sequence or a control byte rather than text. The
 * display side maps its own key codes (xkb keysyms in mwt.c) onto these; the
 * bytes themselves depend only on the key, the modifiers and the terminal's
 * modes, so they are worked out here and tested without a display. */
enum vt_key {
	VT_KEY_UP, VT_KEY_DOWN, VT_KEY_RIGHT, VT_KEY_LEFT, VT_KEY_HOME, VT_KEY_END,
	VT_KEY_BEGIN, /* the keypad's 5 without NumLock */
	VT_KEY_INSERT, VT_KEY_DELETE, VT_KEY_PAGE_UP, VT_KEY_PAGE_DOWN, VT_KEY_MENU,
	VT_KEY_F1, VT_KEY_F2, VT_KEY_F3, VT_KEY_F4, VT_KEY_F5, VT_KEY_F6,
	VT_KEY_F7, VT_KEY_F8, VT_KEY_F9, VT_KEY_F10, VT_KEY_F11, VT_KEY_F12,
	VT_KEY_BACKSPACE, VT_KEY_ENTER, VT_KEY_TAB,
	VT_KEY_BACKTAB, /* ISO_Left_Tab: what Shift+Tab gives on most layouts */
	VT_KEY_ESCAPE,
	VT_KEY_COUNT
};
enum { VT_MOD_SHIFT = 1, VT_MOD_ALT = 2, VT_MOD_CTRL = 4 };

/* The bytes for a key with VT_MOD_* bits, written to buf (not terminated):
 * their count, or 0 for no key or no room. 16 bytes are always enough. */
size_t vt_key(const struct vt *vt, enum vt_key key, unsigned mods, char *buf, size_t cap);

#endif
