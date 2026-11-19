/* test_input.c - keys and mouse reports without a compositor: keymaps compiled
 * from layout names, as a compositor would send them; make check */
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* bytes as C escapes, for the failure message */
static const char *esc(const char *s, size_t n)
{
	static char out[4][256];
	static int k;
	char *o = out[k++ % 4];
	size_t j = 0;
	for (size_t i = 0; i < n && j < 250; i++) {
		unsigned char c = (unsigned char)s[i];
		j += (size_t)snprintf(o + j, 256 - j, c >= 0x20 && c < 0x7f && c != '\\' ?
				      "%c" : "\\x%02x", c);
	}
	o[j] = 0;
	return o;
}

static void check_bytes(const char *file, int line, const char *what, const char *got,
			size_t n, const char *want, size_t wn)
{
	checks++;
	if (n == wn && !memcmp(got, want, n))
		return;
	failures++;
	fprintf(stderr, "%s:%d: FAIL: %s: \"%s\", expected \"%s\"\n", file, line, what,
		esc(got, n), esc(want, wn));
}

/* want is a literal, so sizeof counts an embedded NUL */
#define CHECK_SEND(o, want) do { \
	struct key_out o_ = (o); /* once: a key through compose changes its state */ \
	CHECK(o_.act == DO_SEND); \
	check_bytes(__FILE__, __LINE__, #o, o_.buf, o_.n, want, sizeof want - 1); \
} while (0)

static void nop_write(void *ud, const char *b, size_t n) { (void)ud; (void)b; (void)n; }
static void nop_title(void *ud, const char *s) { (void)ud; (void)s; }

static struct vt *vt;
static struct xkb_context *ctx;

static void mode(const char *seq)
{
	vt_write(vt, seq, strlen(seq));
}

struct kb {
	struct xkb_keymap *km;
	struct xkb_state *st;
};

static struct kb kb_new(const char *layout, const char *variant)
{
	struct xkb_rule_names names = { .layout = layout, .variant = variant };
	struct kb k = { xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS),
			NULL };
	if (!k.km) {
		fprintf(stderr, "test_input: no keymap for layout %s; xkeyboard-config?\n", layout);
		exit(1);
	}
	k.st = xkb_state_new(k.km);
	return k;
}

static void kb_free(struct kb *k)
{
	xkb_state_unref(k->st);
	xkb_keymap_unref(k->km);
}

enum { NUM = 8 }; /* NumLock, next to the VT_MOD_* bits */

/* VT_MOD_* and NUM held, layout group locked, as wl_keyboard.modifiers sets them */
static void hold(struct kb *k, unsigned mods, unsigned group)
{
	xkb_mod_mask_t dep = 0, lock = 0;
	if (mods & VT_MOD_SHIFT)
		dep |= 1u << xkb_keymap_mod_get_index(k->km, XKB_MOD_NAME_SHIFT);
	if (mods & VT_MOD_ALT)
		dep |= 1u << xkb_keymap_mod_get_index(k->km, XKB_MOD_NAME_ALT);
	if (mods & VT_MOD_CTRL)
		dep |= 1u << xkb_keymap_mod_get_index(k->km, XKB_MOD_NAME_CTRL);
	if (mods & NUM)
		lock |= 1u << xkb_keymap_mod_get_index(k->km, XKB_MOD_NAME_NUM);
	xkb_state_update_mask(k->st, dep, 0, lock, 0, 0, group);
}

/* the evdev key with mods, layout group 0, no compose */
static struct key_out key(struct kb *k, int evdev, unsigned mods)
{
	struct key_out o;
	hold(k, mods, 0);
	input_key(k->st, NULL, vt, (xkb_keycode_t)evdev + 8, &o);
	return o;
}

enum { S = VT_MOD_SHIFT, A = VT_MOD_ALT, C = VT_MOD_CTRL };

static void test_vt_key_map(void)
{
	static const struct { xkb_keysym_t sym; enum vt_key key; } map[] = {
		{ XKB_KEY_Up, VT_KEY_UP }, { XKB_KEY_KP_Up, VT_KEY_UP },
		{ XKB_KEY_Down, VT_KEY_DOWN }, { XKB_KEY_KP_Down, VT_KEY_DOWN },
		{ XKB_KEY_Right, VT_KEY_RIGHT }, { XKB_KEY_KP_Right, VT_KEY_RIGHT },
		{ XKB_KEY_Left, VT_KEY_LEFT }, { XKB_KEY_KP_Left, VT_KEY_LEFT },
		{ XKB_KEY_Home, VT_KEY_HOME }, { XKB_KEY_KP_Home, VT_KEY_HOME },
		{ XKB_KEY_End, VT_KEY_END }, { XKB_KEY_KP_End, VT_KEY_END },
		{ XKB_KEY_Insert, VT_KEY_INSERT }, { XKB_KEY_KP_Insert, VT_KEY_INSERT },
		{ XKB_KEY_Delete, VT_KEY_DELETE }, { XKB_KEY_KP_Delete, VT_KEY_DELETE },
		{ XKB_KEY_Prior, VT_KEY_PAGE_UP }, { XKB_KEY_KP_Prior, VT_KEY_PAGE_UP },
		{ XKB_KEY_Next, VT_KEY_PAGE_DOWN }, { XKB_KEY_KP_Next, VT_KEY_PAGE_DOWN },
		{ XKB_KEY_BackSpace, VT_KEY_BACKSPACE },
		{ XKB_KEY_Return, VT_KEY_ENTER }, { XKB_KEY_KP_Enter, VT_KEY_ENTER },
		{ XKB_KEY_Tab, VT_KEY_TAB }, { XKB_KEY_ISO_Left_Tab, VT_KEY_BACKTAB },
		{ XKB_KEY_Escape, VT_KEY_ESCAPE },
		{ XKB_KEY_F1, VT_KEY_F1 }, { XKB_KEY_F5, VT_KEY_F5 }, { XKB_KEY_F12, VT_KEY_F12 },
		/* text, and keys mwt does not encode */
		{ XKB_KEY_a, VT_KEY_COUNT }, { XKB_KEY_space, VT_KEY_COUNT },
		{ XKB_KEY_F13, VT_KEY_COUNT }, { XKB_KEY_KP_5, VT_KEY_COUNT },
		{ XKB_KEY_KP_Begin, VT_KEY_COUNT }, { XKB_KEY_NoSymbol, VT_KEY_COUNT },
	};
	for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
		checks++;
		if (input_vt_key(map[i].sym) != map[i].key) {
			failures++;
			fprintf(stderr, "FAIL: keysym 0x%x is vt_key %d, expected %d\n",
				map[i].sym, input_vt_key(map[i].sym), map[i].key);
		}
	}
	for (xkb_keysym_t s = XKB_KEY_F1; s <= XKB_KEY_F12; s++)
		CHECK((int)input_vt_key(s) == VT_KEY_F1 + (int)(s - XKB_KEY_F1));
}

static void test_text(void)
{
	struct kb k = kb_new("us", NULL);
	CHECK_SEND(key(&k, KEY_A, 0), "a");
	CHECK_SEND(key(&k, KEY_A, S), "A");
	CHECK_SEND(key(&k, KEY_A, A), "\033a");
	CHECK_SEND(key(&k, KEY_A, S | A), "\033A");
	CHECK_SEND(key(&k, KEY_1, S), "!");
	CHECK_SEND(key(&k, KEY_SPACE, 0), " ");
	/* Ctrl makes control bytes, Alt puts ESC before them */
	CHECK_SEND(key(&k, KEY_A, C), "\x01");
	CHECK_SEND(key(&k, KEY_C, C), "\x03");
	CHECK_SEND(key(&k, KEY_Z, C), "\x1a");
	CHECK_SEND(key(&k, KEY_C, C | A), "\033\x03");
	CHECK_SEND(key(&k, KEY_LEFTBRACE, C), "\033");
	CHECK_SEND(key(&k, KEY_BACKSLASH, C), "\x1c");
	/* Ctrl+Shift+letter that is no shortcut: the control byte as without Shift */
	CHECK_SEND(key(&k, KEY_B, C | S), "\x02");
	/* NUL from Ctrl+Space, Ctrl+2 and Ctrl+@; xkbcommon 1.13 gives it itself,
	 * input.c has it for versions that give nothing */
	CHECK_SEND(key(&k, KEY_SPACE, C), "\0");
	CHECK_SEND(key(&k, KEY_2, C), "\0");
	CHECK_SEND(key(&k, KEY_2, C | S), "\0");
	CHECK_SEND(key(&k, KEY_SPACE, C | A), "\033\0");
	/* keys without text send nothing */
	CHECK(key(&k, KEY_LEFTSHIFT, 0).act == DO_NONE);
	CHECK(key(&k, KEY_F13, 0).act == DO_NONE);
	CHECK(key(&k, KEY_CAPSLOCK, 0).act == DO_NONE);
	kb_free(&k);
}

static void test_special_keys(void)
{
	struct kb k = kb_new("us", NULL);
	CHECK_SEND(key(&k, KEY_UP, 0), "\033[A");
	CHECK_SEND(key(&k, KEY_UP, C), "\033[1;5A");
	CHECK_SEND(key(&k, KEY_LEFT, S | A | C), "\033[1;8D");
	CHECK_SEND(key(&k, KEY_ENTER, 0), "\r");
	CHECK_SEND(key(&k, KEY_ENTER, A), "\033\r");
	CHECK_SEND(key(&k, KEY_TAB, 0), "\t");
	CHECK_SEND(key(&k, KEY_TAB, S), "\033[Z"); /* ISO_Left_Tab */
	CHECK_SEND(key(&k, KEY_BACKSPACE, 0), "\x7f");
	CHECK_SEND(key(&k, KEY_BACKSPACE, C), "\b");
	CHECK_SEND(key(&k, KEY_ESC, 0), "\033");
	CHECK_SEND(key(&k, KEY_F1, 0), "\033OP");
	CHECK_SEND(key(&k, KEY_F5, S), "\033[15;2~");
	CHECK_SEND(key(&k, KEY_DELETE, 0), "\033[3~");
	CHECK_SEND(key(&k, KEY_PAGEUP, 0), "\033[5~");
	CHECK_SEND(key(&k, KEY_HOME, 0), "\033[H");
	/* the keypad without NumLock is the cursor block, with it digits */
	CHECK_SEND(key(&k, KEY_KP8, 0), "\033[A");
	CHECK_SEND(key(&k, KEY_KP8, NUM), "8");
	CHECK_SEND(key(&k, KEY_KP9, 0), "\033[5~");
	CHECK_SEND(key(&k, KEY_KPENTER, 0), "\r");
	/* the modes of the terminal: DECCKM, LNM */
	mode("\033[?1h");
	CHECK_SEND(key(&k, KEY_UP, 0), "\033OA");
	CHECK_SEND(key(&k, KEY_UP, C), "\033[1;5A");
	mode("\033[?1l\033[20h");
	CHECK_SEND(key(&k, KEY_ENTER, 0), "\r\n");
	mode("\033[20l");
	CHECK_SEND(key(&k, KEY_ENTER, 0), "\r");
	kb_free(&k);
}

static void test_shortcuts(void)
{
	struct kb k = kb_new("us", NULL);
	CHECK(key(&k, KEY_A, C | S).act == DO_SELECT_ALL);
	CHECK(key(&k, KEY_C, C | S).act == DO_COPY);
	CHECK(key(&k, KEY_V, C | S).act == DO_PASTE);
	CHECK(key(&k, KEY_EQUAL, C | S).act == DO_ZOOM_IN); /* '+' on us */
	CHECK(key(&k, KEY_MINUS, C | S).act == DO_ZOOM_OUT);
	CHECK(key(&k, KEY_0, C | S).act == DO_ZOOM_RESET);
	CHECK(key(&k, KEY_C, C | S | A).act == DO_COPY);
	/* not without Shift, or without Ctrl */
	CHECK_SEND(key(&k, KEY_C, C), "\x03");
	CHECK_SEND(key(&k, KEY_V, S), "V");
	CHECK_SEND(key(&k, KEY_EQUAL, C), "=");

	/* Shift alone: the scrollback and the primary selection */
	CHECK(key(&k, KEY_INSERT, S).act == DO_PASTE_PRIMARY);
	CHECK(key(&k, KEY_PAGEUP, S).act == DO_VIEW_PAGE_UP);
	CHECK(key(&k, KEY_PAGEDOWN, S).act == DO_VIEW_PAGE_DOWN);
	CHECK(key(&k, KEY_HOME, S).act == DO_VIEW_TOP);
	CHECK(key(&k, KEY_END, S).act == DO_VIEW_BOTTOM);
	CHECK(key(&k, KEY_KP9, S).act == DO_VIEW_PAGE_UP);
	CHECK(key(&k, KEY_KP7, S).act == DO_VIEW_TOP);
	CHECK(key(&k, KEY_KP0, S).act == DO_PASTE_PRIMARY);
	/* With NumLock, Shift makes the digit a cursor key again and is used up:
	 * the key without Shift, no paste and no jump in the history */
	CHECK_SEND(key(&k, KEY_KP0, S | NUM), "\033[2~");
	CHECK_SEND(key(&k, KEY_KP7, S | NUM), "\033[H");
	CHECK_SEND(key(&k, KEY_KP1, S | NUM), "\033[F");
	CHECK_SEND(key(&k, KEY_KP9, S | NUM), "\033[5~");
	CHECK_SEND(key(&k, KEY_KP8, S | NUM), "\033[A");
	CHECK_SEND(key(&k, KEY_KP8, S | C | NUM), "\033[1;5A");
	CHECK(key(&k, KEY_KP5, S | NUM).act == DO_NONE); /* KP_Begin */
	/* the keys of their own keep Shift with NumLock */
	CHECK(key(&k, KEY_INSERT, S | NUM).act == DO_PASTE_PRIMARY);
	CHECK(key(&k, KEY_PAGEUP, S | NUM).act == DO_VIEW_PAGE_UP);
	CHECK_SEND(key(&k, KEY_UP, S | NUM), "\033[1;2A");
	CHECK(key(&k, KEY_C, C | S | NUM).act == DO_COPY);
	/* with Ctrl or Alt they go to the program */
	CHECK_SEND(key(&k, KEY_PAGEUP, S | C), "\033[5;6~");
	CHECK_SEND(key(&k, KEY_PAGEUP, S | A), "\033[5;4~");
	CHECK_SEND(key(&k, KEY_INSERT, S | C), "\033[2;6~");
	/* a program that takes the mouse keeps Shift+Home and End, not PgUp */
	mode("\033[?1000h");
	CHECK_SEND(key(&k, KEY_HOME, S), "\033[1;2H");
	CHECK_SEND(key(&k, KEY_END, S), "\033[1;2F");
	CHECK(key(&k, KEY_PAGEUP, S).act == DO_VIEW_PAGE_UP);
	mode("\033[?1000l");
	/* the alternate screen has no scrollback: all four go to the program */
	mode("\033[?1049h");
	CHECK_SEND(key(&k, KEY_PAGEUP, S), "\033[5;2~");
	CHECK_SEND(key(&k, KEY_PAGEDOWN, S), "\033[6;2~");
	CHECK_SEND(key(&k, KEY_HOME, S), "\033[1;2H");
	CHECK_SEND(key(&k, KEY_END, S), "\033[1;2F");
	CHECK(key(&k, KEY_INSERT, S).act == DO_PASTE_PRIMARY);
	mode("\033[?1049l");
	CHECK(key(&k, KEY_PAGEUP, S).act == DO_VIEW_PAGE_UP);
	kb_free(&k);

	/* by the key's base symbol: '+' is a key of its own on de */
	k = kb_new("de", NULL);
	CHECK(key(&k, KEY_RIGHTBRACE, C | S).act == DO_ZOOM_IN);
	CHECK(key(&k, KEY_SLASH, C | S).act == DO_ZOOM_OUT);
	CHECK(key(&k, KEY_0, C | S).act == DO_ZOOM_RESET);
	CHECK(key(&k, KEY_C, C | S).act == DO_COPY);
	/* z and y swap places on de, the shortcut goes by the letter */
	CHECK(key(&k, KEY_A, C | S).act == DO_SELECT_ALL);
	CHECK_SEND(key(&k, KEY_Y, C), "\x1a");
	kb_free(&k);
}

static void test_non_latin(void)
{
	struct kb k = kb_new("ru", NULL);
	CHECK_SEND(key(&k, KEY_C, 0), "с");
	/* Ctrl on a Cyrillic letter: its place on QWERTY */
	CHECK_SEND(key(&k, KEY_C, C), "\x03");
	CHECK_SEND(key(&k, KEY_Z, C), "\x1a");
	CHECK_SEND(key(&k, KEY_C, C | A), "\033\x03");
	CHECK(key(&k, KEY_C, C | S).act == DO_COPY);
	CHECK(key(&k, KEY_V, C | S).act == DO_PASTE);
	CHECK(key(&k, KEY_A, C | S).act == DO_SELECT_ALL);
	/* Alt alone keeps the letter */
	CHECK_SEND(key(&k, KEY_C, A), "\033с");
	kb_free(&k);

	k = kb_new("gr", NULL);
	CHECK_SEND(key(&k, KEY_C, C), "\x03");
	CHECK_SEND(key(&k, KEY_W, C), "\x17"); /* final sigma */
	CHECK_SEND(key(&k, KEY_Q, C), ";"); /* gr has ';' there: ASCII stays */
	CHECK(key(&k, KEY_C, C | S).act == DO_COPY);
	kb_free(&k);

	/* two layouts, the Cyrillic one active: the letter of the Latin one */
	k = kb_new("us,ru", NULL);
	struct key_out o;
	hold(&k, 0, 1);
	input_key(k.st, NULL, vt, KEY_C + 8, &o);
	CHECK_SEND(o, "с");
	hold(&k, C, 1);
	input_key(k.st, NULL, vt, KEY_C + 8, &o);
	CHECK_SEND(o, "\x03");
	hold(&k, C | S, 1);
	input_key(k.st, NULL, vt, KEY_V + 8, &o);
	CHECK(o.act == DO_PASTE);
	CHECK(input_ctrl_latin(k.km, KEY_C + 8, XKB_KEY_Cyrillic_es) == XKB_KEY_c);
	kb_free(&k);

	/* on de the swapped z and y follow the layout, not QWERTY */
	k = kb_new("ru,de", NULL);
	CHECK(input_ctrl_latin(k.km, KEY_Y + 8, XKB_KEY_Cyrillic_en) == XKB_KEY_z);
	kb_free(&k);

	/* letters without a Latin place stay what they are, also with Ctrl */
	k = kb_new("de", NULL);
	CHECK_SEND(key(&k, KEY_SEMICOLON, C), "ö");
	CHECK(input_ctrl_latin(k.km, KEY_SEMICOLON + 8, XKB_KEY_odiaeresis) ==
	      XKB_KEY_odiaeresis);
	CHECK(input_ctrl_latin(k.km, KEY_A + 8, XKB_KEY_a) == XKB_KEY_a);
	CHECK(input_ctrl_latin(k.km, KEY_UP + 8, XKB_KEY_Up) == XKB_KEY_Up);
	kb_free(&k);
}

static void test_compose(void)
{
	struct xkb_compose_table *tab = xkb_compose_table_new_from_locale(ctx,
		"en_US.UTF-8", XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (!tab) {
		failures++;
		fprintf(stderr, "FAIL: no compose table for en_US.UTF-8 (libx11-data?)\n");
		return;
	}
	struct xkb_compose_state *cs = xkb_compose_state_new(tab, XKB_COMPOSE_STATE_NO_FLAGS);
	struct kb k = kb_new("de", NULL);
	struct key_out o;
#define PRESS(evdev, mods) (hold(&k, mods, 0), input_key(k.st, cs, vt, (evdev) + 8, &o), o)
	/* the dead circumflex left of 1, then a letter */
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_A, 0), "â");
	CHECK_SEND(PRESS(KEY_A, 0), "a");
	CHECK(PRESS(KEY_EQUAL, 0).act == DO_NONE); /* dead acute */
	CHECK_SEND(PRESS(KEY_E, S), "É");
	/* a key that does not go through compose ends what the dead key began:
	 * Ctrl+C must interrupt, and the next letter be plain */
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_C, C), "\x03");
	CHECK_SEND(PRESS(KEY_A, 0), "a");
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_UP, 0), "\033[A");
	CHECK_SEND(PRESS(KEY_A, 0), "a");
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_A, A), "\033a");
	CHECK_SEND(PRESS(KEY_A, 0), "a");
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK(PRESS(KEY_C, C | S).act == DO_COPY);
	CHECK_SEND(PRESS(KEY_A, 0), "a");
	/* a modifier on its own keeps the dead key waiting, as Shift for É above */
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK(PRESS(KEY_LEFTSHIFT, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_O, 0), "ô");
	/* twice the dead key: the character itself */
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_GRAVE, 0), "^");
	/* no sequence: both keys are swallowed, the next is plain */
	CHECK(PRESS(KEY_GRAVE, 0).act == DO_NONE);
	CHECK(PRESS(KEY_Q, 0).act == DO_NONE);
	CHECK_SEND(PRESS(KEY_Q, 0), "q");
#undef PRESS
	kb_free(&k);
	xkb_compose_state_unref(cs);
	xkb_compose_table_unref(tab);
}

static void test_mods(void)
{
	struct kb k = kb_new("us", NULL);
	CHECK(input_mods(NULL) == 0);
	hold(&k, 0, 0);
	CHECK(input_mods(k.st) == 0);
	hold(&k, S | C, 0);
	CHECK(input_mods(k.st) == (S | C));
	hold(&k, A | NUM, 0);
	CHECK(input_mods(k.st) == A);
	kb_free(&k);
}

/* the report as a string, or "" when there is none */
static const char *report(unsigned m, int code, unsigned mods, bool press, int col, int row)
{
	static char buf[64];
	size_t n = input_mouse(m, code, mods, press, col, row, buf, sizeof buf);
	buf[n] = 0;
	return buf;
}

static void test_mouse(void)
{
	enum { BTN = MODE_MOUSE_BTN, DRAG = MODE_MOUSE_DRAG, ANY = MODE_MOUSE_ANY,
	       SGR = MODE_MOUSE_SGR };
	CHECK(!input_mouse_mode(0, 0));
	CHECK(!input_mouse_mode(SGR, 0));
	CHECK(input_mouse_mode(BTN, 0));
	CHECK(input_mouse_mode(DRAG, A | C));
	CHECK(input_mouse_mode(ANY | SGR, 0));
	CHECK(!input_mouse_mode(BTN, S));
	CHECK(!input_mouse_mode(ANY, S | C));

	/* X10 encoding: 32 + code, 32 + column, 32 + row, from 1 */
	CHECK(!strcmp(report(BTN, 0, 0, true, 0, 0), "\033[M !!"));
	CHECK(!strcmp(report(BTN, 2, 0, true, 9, 4), "\033[M\"*%"));
	/* a release is button 3, whichever went up */
	CHECK(!strcmp(report(BTN, 2, 0, false, 0, 0), "\033[M#!!"));
	CHECK(!strcmp(report(BTN, 32, 0, false, 0, 0), "\033[MC!!"));
	/* Shift 4, Alt 8, Ctrl 16 */
	CHECK(!strcmp(report(BTN, 0, S, true, 0, 0), "\033[M$!!"));
	CHECK(!strcmp(report(BTN, 1, A | C, true, 0, 0), "\033[M9!!"));
	CHECK(!strcmp(report(BTN, 64, 0, true, 0, 0), "\033[M`!!"));
	CHECK(!strcmp(report(BTN, 65, C, true, 0, 0), "\033[Mq!!"));
	/* up to cell 223, the last byte 255; past it nothing */
	CHECK(!strcmp(report(BTN, 0, 0, true, 222, 222), "\033[M \xff\xff"));
	CHECK(!strcmp(report(BTN, 0, 0, true, 223, 0), ""));
	CHECK(!strcmp(report(BTN, 0, 0, true, 0, 223), ""));

	/* SGR: decimal, the release keeps the button and ends in m */
	CHECK(!strcmp(report(BTN | SGR, 0, 0, true, 0, 0), "\033[<0;1;1M"));
	CHECK(!strcmp(report(BTN | SGR, 2, 0, false, 9, 4), "\033[<2;10;5m"));
	CHECK(!strcmp(report(BTN | SGR, 35, S | A | C, true, 0, 0), "\033[<63;1;1M"));
	CHECK(!strcmp(report(BTN | SGR, 64, 0, true, 499, 299), "\033[<64;500;300M"));
	CHECK(!strcmp(report(BTN | SGR, 0, 0, true, 9999, 9999), "\033[<0;10000;10000M"));

	/* no room: nothing rather than a cut report */
	char small[16];
	CHECK(input_mouse(BTN | SGR, 0, 0, true, 0, 0, small, 9) == 0);
	CHECK(input_mouse(BTN | SGR, 0, 0, true, 0, 0, small, 10) == 9);
	CHECK(input_mouse(BTN | SGR, 0, 0, true, 0, 0, small, 0) == 0);
	CHECK(input_mouse(BTN, 0, 0, true, 0, 0, small, 6) == 0);
	CHECK(input_mouse(BTN, 0, 0, true, 0, 0, small, 7) == 6);
}

static void test_motion(void)
{
	enum { BTN = MODE_MOUSE_BTN, DRAG = MODE_MOUSE_DRAG, ANY = MODE_MOUSE_ANY };
	/* only to another cell */
	CHECK(input_motion(ANY, 0, false) == -1);
	/* 1000 reports no moves, 1002 only with a button, 1003 always */
	CHECK(input_motion(BTN, 1, true) == -1);
	CHECK(input_motion(DRAG, 0, true) == -1);
	CHECK(input_motion(DRAG, 1, true) == 32);
	CHECK(input_motion(ANY, 0, true) == 35);
	CHECK(input_motion(ANY | DRAG, 0, true) == 35);
	/* with several held: left before middle before right */
	CHECK(input_motion(DRAG, 2, true) == 33);
	CHECK(input_motion(DRAG, 4, true) == 34);
	CHECK(input_motion(DRAG, 6, true) == 33);
	CHECK(input_motion(DRAG, 5, true) == 32);
	CHECK(input_motion(DRAG, 7, true) == 32);
}

static void test_cell(void)
{
	int c, r;
	/* scale 2, padding 4, cells 10x20 buffer pixels, an 80x24 grid */
#define CELL(x, y) input_cell(x, y, 2, 4, 10, 20, 80, 24, &c, &r)
	CELL(0, 0);
	CHECK(c == 0 && r == 0);
	CELL(-5, -5);
	CHECK(c == 0 && r == 0);
	CELL(6.9, 11.9); /* 13 and 23 buffer pixels: still the first cell */
	CHECK(c == 0 && r == 0);
	CELL(7, 12);
	CHECK(c == 1 && r == 1);
	CELL(401.9, 241.9);
	CHECK(c == 79 && r == 23);
	CELL(1e6, 1e6);
	CHECK(c == 79 && r == 23);
#undef CELL
	input_cell(0, 0, 1, 0, 7, 15, 1, 1, &c, &r);
	CHECK(c == 0 && r == 0);
	input_cell(100, 100, 1, 0, 7, 15, 1, 1, &c, &r);
	CHECK(c == 0 && r == 0);
}

int main(void)
{
	struct vt_callbacks cb = { .write = nop_write, .title = nop_title };
	vt = vt_new(80, 24, 100, &cb);
	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!vt || !ctx) {
		fprintf(stderr, "test_input: no vt or xkb context\n");
		return 1;
	}
	test_vt_key_map();
	test_text();
	test_special_keys();
	test_shortcuts();
	test_non_latin();
	test_compose();
	test_mods();
	test_mouse();
	test_motion();
	test_cell();
	vt_free(vt);
	xkb_context_unref(ctx);
	printf("test_input: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
