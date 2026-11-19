/* input.c - keys and mouse reports; see input.h */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <string.h>

#include "input.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

static bool mod_active(struct xkb_state *st, const char *name)
{
	return xkb_state_mod_name_is_active(st, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

unsigned input_mods(struct xkb_state *st)
{
	if (!st)
		return 0;
	return (mod_active(st, XKB_MOD_NAME_SHIFT) ? VT_MOD_SHIFT : 0) |
	       (mod_active(st, XKB_MOD_NAME_ALT) ? VT_MOD_ALT : 0) |
	       (mod_active(st, XKB_MOD_NAME_CTRL) ? VT_MOD_CTRL : 0);
}

/* The keypad ones with NumLock off are the same keys: see CAVEATS.md on the
 * keypad mode. */
enum vt_key input_vt_key(xkb_keysym_t sym)
{
	switch (sym) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up: return VT_KEY_UP;
	case XKB_KEY_Down: case XKB_KEY_KP_Down: return VT_KEY_DOWN;
	case XKB_KEY_Right: case XKB_KEY_KP_Right: return VT_KEY_RIGHT;
	case XKB_KEY_Left: case XKB_KEY_KP_Left: return VT_KEY_LEFT;
	case XKB_KEY_Home: case XKB_KEY_KP_Home: return VT_KEY_HOME;
	case XKB_KEY_End: case XKB_KEY_KP_End: return VT_KEY_END;
	case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return VT_KEY_INSERT;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return VT_KEY_DELETE;
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior: return VT_KEY_PAGE_UP;
	case XKB_KEY_Next: case XKB_KEY_KP_Next: return VT_KEY_PAGE_DOWN;
	case XKB_KEY_BackSpace: return VT_KEY_BACKSPACE;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: return VT_KEY_ENTER;
	case XKB_KEY_Tab: return VT_KEY_TAB;
	case XKB_KEY_ISO_Left_Tab: return VT_KEY_BACKTAB;
	case XKB_KEY_Escape: return VT_KEY_ESCAPE;
	case XKB_KEY_KP_Begin: return VT_KEY_BEGIN;
	case XKB_KEY_Menu: return VT_KEY_MENU;
	}
	if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
		return (enum vt_key)(VT_KEY_F1 + (sym - XKB_KEY_F1));
	return VT_KEY_COUNT;
}

/* Without Ctrl+C a Russian or Greek layout would interrupt nothing. Keys without
 * a Latin place keep what they are, like the German umlauts. */
xkb_keysym_t input_ctrl_latin(struct xkb_keymap *km, xkb_keycode_t code, xkb_keysym_t sym)
{
	if (xkb_keysym_to_utf32(sym) < 0x80) /* ASCII, or no text at all */
		return sym;
	xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(km, code);
	for (xkb_layout_index_t l = 0; l < layouts; l++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(km, code, l, 0, &syms) == 1 &&
		    syms[0] >= XKB_KEY_a && syms[0] <= XKB_KEY_z)
			return syms[0];
	}
	static const struct { int first; const char *row; } rows[] = {
		{ KEY_Q, "qwertyuiop" }, { KEY_A, "asdfghjkl" }, { KEY_Z, "zxcvbnm" },
	};
	int key = (int)code - 8;
	for (size_t i = 0; i < LEN(rows); i++) {
		int k = key - rows[i].first;
		if (k >= 0 && k < (int)strlen(rows[i].row))
			return (xkb_keysym_t)rows[i].row[k];
	}
	return sym;
}

/* the first keysym of the key's first level, in the layout now active */
static xkb_keysym_t base_sym(struct xkb_state *st, xkb_keycode_t code)
{
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(st, code);
	if (xkb_keymap_key_get_syms_by_level(xkb_state_get_keymap(st), code, layout, 0,
					     &syms) > 0)
		return syms[0];
	return XKB_KEY_NoSymbol;
}

static void send(struct key_out *out, bool alt, const char *s, size_t n)
{
	size_t o = 0;
	if (n + 1 > sizeof out->buf)
		return;
	if (alt)
		out->buf[o++] = '\033';
	memcpy(out->buf + o, s, n);
	out->act = DO_SEND;
	out->n = o + n;
}

static enum key_do shortcut(xkb_keysym_t latin)
{
	switch (latin) {
	case XKB_KEY_a: return DO_SELECT_ALL;
	case XKB_KEY_c: return DO_COPY;
	case XKB_KEY_v: return DO_PASTE;
	case XKB_KEY_plus: case XKB_KEY_equal: return DO_ZOOM_IN;
	case XKB_KEY_minus: return DO_ZOOM_OUT;
	case XKB_KEY_0: return DO_ZOOM_RESET;
	}
	return DO_NONE;
}

/* Shift alone: the scrollback and the primary selection. The alternate screen
 * has no scrollback, these go to the program there; Home and End also go to one
 * that takes the mouse (not by input_mouse_mode: Shift always turns that off). */
static enum key_do shift_key(xkb_keysym_t sym, unsigned vt_mode)
{
	switch (sym) {
	case XKB_KEY_Insert: case XKB_KEY_KP_Insert:
		return DO_PASTE_PRIMARY;
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior:
		return vt_mode & MODE_ALTSCREEN ? DO_NONE : DO_VIEW_PAGE_UP;
	case XKB_KEY_Next: case XKB_KEY_KP_Next:
		return vt_mode & MODE_ALTSCREEN ? DO_NONE : DO_VIEW_PAGE_DOWN;
	}
	if (vt_mode & (MODE_ALTSCREEN | MODE_MOUSE))
		return DO_NONE;
	switch (sym) {
	case XKB_KEY_Home: case XKB_KEY_KP_Home: return DO_VIEW_TOP;
	case XKB_KEY_End: case XKB_KEY_KP_End: return DO_VIEW_BOTTOM;
	}
	return DO_NONE;
}

/* With NumLock a keypad key is a digit, and Shift turns it back into its cursor
 * key. That Shift does not paste or scroll, as xterm, whose Shift+Insert and
 * Shift+PgUp are the keys of their own; the key still goes out with Shift. xkb
 * counts Shift as used without NumLock too, where it changes nothing. */
static bool keypad_shift(struct xkb_state *st, xkb_keycode_t code)
{
	struct xkb_keymap *km = xkb_state_get_keymap(st);
	return xkb_state_mod_name_is_active(st, XKB_MOD_NAME_NUM, XKB_STATE_MODS_EFFECTIVE) > 0 &&
	       xkb_state_mod_index_is_consumed(st, code,
					       xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT)) > 0;
}

/* true when compose took the key */
static bool key_bytes(struct xkb_state *st, struct xkb_compose_state *compose,
		      const struct vt *vt, xkb_keycode_t code, struct key_out *out)
{
	xkb_keysym_t sym = xkb_state_key_get_one_sym(st, code);
	unsigned mods = input_mods(st);
	bool ctrl = mods & VT_MOD_CTRL, alt = mods & VT_MOD_ALT, shift = mods & VT_MOD_SHIFT;
	unsigned kmods = mods;

	if (ctrl && shift) {
		out->act = shortcut(input_ctrl_latin(xkb_state_get_keymap(st), code,
						     base_sym(st, code)));
		if (out->act != DO_NONE)
			return false;
	}
	if (shift && !ctrl && !alt && !keypad_shift(st, code)) {
		out->act = shift_key(sym, vt->mode);
		if (out->act != DO_NONE)
			return false;
	}

	enum vt_key key = input_vt_key(sym);
	/* Shift+F1 to F12, as kf13 to kf24 of xterm's terminfo and of mwt.info;
	 * xterm itself sends CSI 25 ~ and on for them */
	if (sym >= XKB_KEY_F13 && sym <= XKB_KEY_F24) {
		key = (enum vt_key)(VT_KEY_F1 + (sym - XKB_KEY_F13));
		kmods |= VT_MOD_SHIFT;
	}
	if (key != VT_KEY_COUNT) {
		out->n = vt_key(vt, key, kmods, out->buf, sizeof out->buf);
		out->act = DO_SEND;
		return false;
	}

	if (ctrl) {
		xkb_keysym_t latin = input_ctrl_latin(xkb_state_get_keymap(st), code, sym);
		if (latin != sym && latin >= XKB_KEY_a && latin <= XKB_KEY_z) {
			char c = (char)(latin & 0x1f);
			send(out, alt, &c, 1);
			return false;
		}
		if (sym == XKB_KEY_question) { /* DEL, as xterm */
			send(out, alt, "\x7f", 1);
			return false;
		}
	}

	char buf[64];
	int n = 0;
	bool fed = false;
	if (compose && !ctrl && !alt) {
		fed = true;
		xkb_compose_state_feed(compose, sym);
		switch (xkb_compose_state_get_status(compose)) {
		case XKB_COMPOSE_COMPOSING:
			return true;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(compose);
			return true;
		case XKB_COMPOSE_COMPOSED:
			n = xkb_compose_state_get_utf8(compose, buf, sizeof buf);
			xkb_compose_state_reset(compose);
			break;
		case XKB_COMPOSE_NOTHING:
			break;
		}
	}
	if (!n)
		n = xkb_state_key_get_utf8(st, code, buf, sizeof buf);
	if (n <= 0) {
		if (ctrl && (sym == XKB_KEY_space || sym == XKB_KEY_at || sym == XKB_KEY_2))
			send(out, alt, "", 1); /* NUL */
		return fed;
	}
	send(out, alt, buf, MIN((size_t)n, sizeof buf - 1));
	return fed;
}

/* Without the reset that dead key would still change a letter typed much later,
 * and would swallow a Ctrl+C. */
void input_key(struct xkb_state *st, struct xkb_compose_state *compose,
	       const struct vt *vt, xkb_keycode_t code, struct key_out *out)
{
	out->act = DO_NONE;
	out->n = 0;
	if (!key_bytes(st, compose, vt, code, out) && compose)
		xkb_compose_state_reset(compose);
}

bool input_mouse_mode(unsigned vt_mode, unsigned mods)
{
	return (vt_mode & MODE_MOUSE) && !(mods & VT_MOD_SHIFT);
}

size_t input_mouse(unsigned vt_mode, int code, unsigned mods, bool press, int col,
		   int row, char *buf, size_t cap)
{
	code |= (mods & VT_MOD_SHIFT ? 4 : 0) | (mods & VT_MOD_ALT ? 8 : 0) |
		(mods & VT_MOD_CTRL ? 16 : 0);
	int x = col + 1, y = row + 1, n;
	if (vt_mode & MODE_MOUSE_SGR) {
		n = snprintf(buf, cap, "\033[<%d;%d;%d%c", code, x, y, press ? 'M' : 'm');
	} else {
		if (!press)
			code = (code & (4 | 8 | 16)) | 3; /* button 3, the modifiers kept */
		/* snprintf counts the NUL of %c, so a 0 byte does not cut it */
		n = snprintf(buf, cap, "\033[M%c%c%c", 32 + code, x > 223 ? 0 : 32 + x,
			     y > 223 ? 0 : 32 + y);
	}
	return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

int input_button_bit(uint32_t evdev, int *code)
{
	static const uint32_t order[] = {
		BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK,
	};
	for (int i = 0; i < (int)LEN(order); i++)
		if (order[i] == evdev) {
			*code = i < 3 ? i : 128 + i - 3;
			return i;
		}
	return -1;
}

bool input_button(struct buttons *bs, int b, bool press, bool mouse)
{
	unsigned bit = 1u << b;
	if (b > 2 && press && !mouse)
		return false;
	bool to_program = press ? mouse : (bs->program & bit) != 0;
	if (press) {
		bs->held |= bit;
		bs->program = mouse ? bs->program | bit : bs->program & ~bit;
	} else {
		bs->held &= ~bit;
		bs->program &= ~bit;
	}
	return to_program;
}

bool input_moved(unsigned vt_mode, int col, int row, int ocol, int orow)
{
	if (!(vt_mode & MODE_MOUSE_SGR)) {
		col = MIN(col, 223);
		row = MIN(row, 223);
		ocol = MIN(ocol, 223);
		orow = MIN(orow, 223);
	}
	return col != ocol || row != orow;
}

size_t input_button_report(unsigned vt_mode, int code, unsigned mods, bool press,
			   int col, int row, char *buf, size_t cap)
{
	if (!press && !(vt_mode & MODE_MOUSE))
		return 0;
	return input_mouse(vt_mode, code, mods, press, col, row, buf, cap);
}

enum wheel_do input_wheel(unsigned vt_mode, unsigned mods, bool sideways)
{
	if (input_mouse_mode(vt_mode, mods))
		return WHEEL_REPORT;
	if (sideways)
		return WHEEL_NONE;
	if (vt_mode & MODE_ALTSCREEN)
		return vt_mode & MODE_MOUSE ? WHEEL_NONE : WHEEL_KEYS;
	return WHEEL_SCROLL;
}

int input_wheel_code(bool sideways, bool positive)
{
	return 64 + (sideways ? 2 : 0) + (positive ? 1 : 0);
}

bool input_move(const struct buttons *bs, bool mouse)
{
	return bs->held ? (bs->held & bs->program) != 0 : mouse;
}

int input_motion(unsigned vt_mode, unsigned buttons, bool moved)
{
	if (!moved || !((vt_mode & MODE_MOUSE_ANY) || ((vt_mode & MODE_MOUSE_DRAG) && buttons)))
		return -1;
	for (int i = 0; i < 7; i++)
		if (buttons & 1u << i)
			return 32 + (i < 3 ? i : 128 + i - 3);
	return 32 + 3;
}

void input_cell(double px, double py, int scale, int pad, int cw, int ch, int cols,
		int rows, int *col, int *row)
{
	int x = (int)(px * scale) - pad, y = (int)(py * scale) - pad;
	*col = x < 0 ? 0 : MIN(x / cw, cols - 1);
	*row = y < 0 ? 0 : MIN(y / ch, rows - 1);
}
