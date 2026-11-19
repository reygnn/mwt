/* input.h - what keys and the mouse send, without Wayland: mwt.c feeds it the
 * xkb state and does what it says, test_input checks it headless. */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "vt.h"

/* What a key press asks mwt to do. */
enum key_do {
	DO_NONE,          /* nothing, or a dead key waiting for the next */
	DO_SEND,          /* buf, n bytes, to the program */
	DO_SELECT_ALL,    /* Ctrl+Shift+A */
	DO_COPY,          /* Ctrl+Shift+C */
	DO_PASTE,         /* Ctrl+Shift+V, the clipboard */
	DO_PASTE_PRIMARY, /* Shift+Insert */
	DO_ZOOM_IN, DO_ZOOM_OUT, DO_ZOOM_RESET,
	DO_VIEW_PAGE_UP, DO_VIEW_PAGE_DOWN, /* Shift+PgUp, Shift+PgDn */
	DO_VIEW_TOP, DO_VIEW_BOTTOM,        /* Shift+Home, Shift+End */
};

struct key_out {
	enum key_do act;
	size_t n;
	char buf[80];
};

/* VT_MOD_* bits of what is held in st; 0 without a state. */
unsigned input_mods(struct xkb_state *st);

/* The vt_key() a keysym stands for, or VT_KEY_COUNT. */
enum vt_key input_vt_key(xkb_keysym_t sym);

/* With Ctrl, the Latin letter a key of a non-Latin layout means: from another
 * layout of the keymap, else from its place on QWERTY; sym when there is none. */
xkb_keysym_t input_ctrl_latin(struct xkb_keymap *km, xkb_keycode_t code, xkb_keysym_t sym);

/* A press of code in st, through compose when it is not NULL. The modes of vt
 * decide the bytes of special keys and which shortcuts apply. A key that does
 * not go through compose ends what a dead key began. */
void input_key(struct xkb_state *st, struct xkb_compose_state *compose,
	       const struct vt *vt, xkb_keycode_t code, struct key_out *out);

/* Whether the mouse goes to the program: a mouse mode is on, and Shift is not
 * held, which takes it back for selecting. */
bool input_mouse_mode(unsigned vt_mode, unsigned mods);

/* The report of button code (0 to 2, 3 for none, +32 for a move, 64 and 65 for
 * the wheel) with VT_MOD_* mods at the cell col, row counted from 0: its length,
 * or 0 when it cannot be sent, past cell 223 without SGR or no room in buf. */
size_t input_mouse(unsigned vt_mode, int code, unsigned mods, bool press, int col,
		   int row, char *buf, size_t cap);

/* Who a button's events go to, decided at its press: the release goes there
 * too, whatever Shift does in between. Bit b for button b. */
struct buttons {
	unsigned held;
	unsigned program; /* of those, the ones whose press went to the program */
};

/* A press or release of button b, with the mouse the program's now (mouse) or
 * not: true when it goes to the program. */
bool input_button(struct buttons *bs, int b, bool press, bool mouse);

/* Whether a move goes to the program: while it holds a button, or while none
 * is held and the mouse is its. */
bool input_move(const struct buttons *bs, bool mouse);

/* The code to report for a move to another cell with buttons (bit 0 left, 1
 * middle, 2 right) held, or -1 when the mode does not report it. */
int input_motion(unsigned vt_mode, unsigned buttons, bool moved);

/* The cell at the pointer, px, py in logical pixels, clamped to the grid. */
void input_cell(double px, double py, int scale, int pad, int cw, int ch, int cols,
		int rows, int *col, int *row);

#endif
