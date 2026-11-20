/* input.h - what keys and the mouse send, without Wayland: mwt.c feeds it the
 * xkb state and does what it says, test_input checks it headless. */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/* The keyboard left: every key counts as let go, as wl_keyboard.leave says;
 * Caps Lock, NumLock and the layout stay. */
void input_leave(struct xkb_state *st);

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

/* The report of button code (0 to 2, 128 to 131 for the side buttons, 3 for
 * none, +32 for a move, 64 to 67 for the wheel) with VT_MOD_* mods at the cell
 * col, row counted from 0: its length, or 0 when buf has no room. The X10 form
 * has a byte for the first 223 cells and sends 0 from cell 223 on, counted from
 * 0; its release is button 3. Both as xterm. */
size_t input_mouse(unsigned vt_mode, int code, unsigned mods, bool press, int col,
		   int row, char *buf, size_t cap);

/* The bit of an evdev button in struct buttons, its code in reports in *code:
 * left, middle and right 0 to 2, the side buttons 8 to 11 3 to 6 with codes 128
 * to 131, as xterm; -1 for one that is not reported. */
int input_button_bit(uint32_t evdev, int *code);

/* Who a button's events go to, decided at its press: the release goes there
 * too, whatever Shift does in between. Bit b for button b. */
struct buttons {
	unsigned held;
	unsigned program; /* of those, the ones whose press went to the program */
	unsigned menu;    /* presses the context menu took, kept by mwt.c: their
			   * release goes nowhere else */
};

/* A press or release of button b, with the mouse the program's now (mouse) or
 * not: true when it goes to the program. A side button that does not is not
 * held: it does nothing but for the program. */
bool input_button(struct buttons *bs, int b, bool press, bool mouse);

/* Whether a move goes to the program: while it holds a button, or while none
 * is held and the mouse is its. */
bool input_move(const struct buttons *bs, bool mouse);

/* Whether a move from cell ocol, orow to col, row changes the report: the X10
 * form says the same for every cell from 223 on, counted from 0. */
bool input_moved(unsigned vt_mode, int col, int row, int ocol, int orow);

/* The report of a press or release that goes to the program, as input_mouse:
 * none for a release once the program has turned the mouse off, as xterm. */
size_t input_button_report(unsigned vt_mode, int code, unsigned mods, bool press,
			   int col, int row, char *buf, size_t cap);

/* What a step of the wheel does: reports to a program that has the mouse; arrow
 * keys in the alternate screen, where there is no history; scrolls the
 * history. With Shift, which takes the mouse back, nothing in the alternate
 * screen of a program that has the mouse: no history, and the keys are not
 * asked for. Sideways it only reports. */
enum wheel_do { WHEEL_REPORT, WHEEL_KEYS, WHEEL_SCROLL, WHEEL_NONE };
enum wheel_do input_wheel(unsigned vt_mode, unsigned mods, bool sideways);

/* Its code: 64 up, 65 down, 66 left, 67 right, as xterm. */
int input_wheel_code(bool sideways, bool positive);

/* The wheel and the touchpad within one wl_pointer.frame. A wheel's notches
 * (axis_discrete) and the same move in pixels (axis) may come in either order
 * by the spec; wlroots, mutter and KWin send the notch first. So the frame is
 * decided at its end: notches where there are any, else the distance, which
 * builds up over frames to a step every step pixels. Vertical 0, horizontal 1. */
struct input_axis {
	double value[2];   /* this frame's pixels */
	int discrete[2];   /* this frame's notches */
	bool notched[2];   /* a notch came in this frame */
	bool stopped[2];   /* axis_stop came in this frame */
	double acc[2];     /* distance over frames, not yet a step */
};
void input_axis_value(struct input_axis *ax, unsigned axis, double v);
void input_axis_discrete(struct input_axis *ax, unsigned axis, int n);
/* the finger left the touchpad: what built up goes, once this frame is done */
void input_axis_stop(struct input_axis *ax, unsigned axis);
/* The frame's steps per axis, positive down or right, at most 10 either way;
 * the frame starts afresh. */
void input_axis_frame(struct input_axis *ax, double step, int steps[2]);

/* The code to report for a move to another cell with buttons held (bits as in
 * struct buttons; left before middle before right before the side buttons, as
 * xterm), or -1 when the mode does not report it. */
int input_motion(unsigned vt_mode, unsigned buttons, bool moved);

/* The cell at the pointer, px, py in logical pixels, clamped to the grid. */
void input_cell(double px, double py, int scale, int pad, int cw, int ch, int cols,
		int rows, int *col, int *row);

#endif
