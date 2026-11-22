/* mwt - minimal Wayland terminal
 *
 * One window, one pty, xdg-shell only. Server-side decorations via
 * xdg-decoration, clipboard via wl_data_device, primary selection via
 * primary-selection-unstable-v1. Rendering into wl_shm buffers, only the
 * rows that changed are redrawn. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"

#include "config.h"
#include "rc.h"
#include "version.h"
#include "box.h"
#include "font.h"
#include "input.h"
#include "link.h"
#include "paste.h"
#include "sel.h"
#include "shell.h"
#include "util.h"
#include "vt.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

/* a mouse pointer image on a surface of its own */
struct pcursor {
	struct wl_cursor_image *image;
	struct wl_surface *surface;
	struct wp_viewport *viewport;
	int hx, hy; /* hotspot, logical */
};

struct buffer {
	struct wl_buffer *wb;
	uint32_t *data;
	int w, h;
	size_t size;
	bool busy;
};

/* a clipboard transfer to one reader, fed from the event loop */
struct xfer {
	int fd;
	char *data;
	size_t len, off;
};
#define MAX_XFERS 16


static struct {
	/* globals */
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	uint32_t seat_name;        /* its registry name, for global_remove */
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct wl_data_device_manager *ddm;
	struct wl_data_device *ddev;
	struct zwp_primary_selection_device_manager_v1 *psm;
	struct zwp_primary_selection_device_v1 *pdev;
	struct wp_viewporter *viewporter;
	struct xdg_activation_v1 *activation;
	bool bell_asked; /* BEL since the focus went: once is enough */

	/* window */
	struct wl_surface *surface;
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wl_callback *frame_cb;
	struct buffer bufs[2], *last;
	uint8_t *differ;   /* per pixel row: may differ between the two buffers */
	int differ_h;
	int width, height; /* logical */
	int scale;
	int cfg_w, cfg_h;
	bool cfg_fixed;    /* maximized, fullscreen or tiled: use size as given */
	bool configured, focused, running, need_redraw;
	bool acked;        /* a configure acked and not yet committed */
	int bounds_w, bounds_h; /* configure_bounds: the most the first size may be */
	int min_w, min_h;  /* the last min size sent */
	bool need_full;    /* the grid moved in the window: next frame from scratch */
	char title[VT_OSC_MAX]; /* the last one the program set, to send once a turn */
	bool title_new;
	int init_cols, init_rows;

	/* font and grid, in buffer pixels */
	struct font *font;
	const char *font_spec;
	double font_pt, font_pt0;
	int cw, ch, ascent, pad;
	uint32_t palette[256]; /* 0 to 15 from col, the rest fixed */
	struct rc_colors col; /* config.h, then mwtrc */

	/* terminal */
	struct vt *vt;
	int pty;
	struct child child; /* a pidfd: the shell can end while jobs keep the pty open */
	struct out out;     /* what waits for the pty */
	bool was_alt;
	int last_cx, last_cy;
	bool last_shown;

	/* keyboard */
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	struct xkb_compose_state *compose;
	struct wl_registry *registry;
	int repeat_fd, repeat_rate, repeat_delay;
	xkb_keycode_t repeat_key;
	uint32_t serial;

	/* pointer */
	struct wl_cursor_theme *cursor_theme;
	struct pcursor text, hand; /* the hand over a link, if the theme has one */
	struct pcursor *shown;     /* the one set since the last enter, or NULL */
	bool cursor_retry;        /* the last load failed: try again on the next enter */
	uint32_t enter_serial;
	double px, py;
	int mcol, mrow;
	struct buttons buttons;
	struct input_axis axis; /* the wheel and touchpad, a wl_pointer.frame at a time */
	struct clicks clicks;
	struct link link;   /* the URL under the pointer, shown while Ctrl is held */
	bool link_on;
	bool link_pressed;  /* button 1 opened a link: its release selects nothing */
	char *menu_link;    /* the URL the menu was opened on, for Open link */

	struct sel sel;
	char *clip_text, *prim_text;
	struct wl_data_source *clip_src;
	struct zwp_primary_selection_source_v1 *prim_src;
	struct wl_data_offer *clip_offer, *dnd_offer;
	struct zwp_primary_selection_offer_v1 *prim_offer;
	int paste_fd;
	char *paste_buf;
	size_t paste_len, paste_cap;
	bool paste_latin1;
	struct xfer xfers[MAX_XFERS];
	int nxfers;

	/* context menu, a popup with its own surface and buffer */
	struct {
		struct wl_surface *surface;
		struct xdg_surface *xsurface;
		struct xdg_popup *popup;
		struct buffer bufs[2]; /* a compositor may hold on to the one it shows */
		int w, h; /* logical */
		int row_h; /* buffer pixels */
		int hover, pressed; /* item, or -1 */
		int first; /* the first item shown: Open link only on a link */
		bool configured, dirty;
	} menu;
	struct wl_surface *ptr_surface; /* the surface the pointer is on */
} t = { .pty = -1, .repeat_fd = -1, .paste_fd = -1, .child = { 0, -1 }, .scale = 1 };

static void link_hover(void);

static void die(const char *msg)
{
	fprintf(stderr, "mwt: %s\n", msg);
	exit(1);
}

/* die, with what errno says */
static void die_errno(const char *msg)
{
	fprintf(stderr, "mwt: %s: %s\n", msg, strerror(errno));
	exit(1);
}

/* ---- pty output ------------------------------------------------------- */

#define REPLY_BACKLOG_MAX (1024 * 1024) /* replies waiting for the pty */
/* Repeats a stalled event loop owes are sent at once, at most this many: a
 * held key after a long stall should not flood the program. */
#define REPEAT_BURST_MAX 32u
/* Alpha of the foreground over the background, of 255: dim text, and the menu's
 * disabled items and shortcuts. */
#define DIM_ALPHA 140
#define MENU_DISABLED_ALPHA 110
#define MENU_SHORTCUT_ALPHA 150
#define PASTE_MAX_MIB 64                 /* a larger paste is dropped whole */
#define PASTE_MAX ((size_t)PASTE_MAX_MIB << 20)
#define PASTE_TURN_MAX (1024 * 1024)    /* read in one turn of the event loop */
/* pty output in one turn: this long or this much, whichever first (read_pty) */
#define READ_TURN_MS 16
#define READ_TURN_MAX (1024 * 1024)
/* The pause after a short read in a flood (shell_read): yes and seq take half
 * the CPU and end 25 to 35 % later. Shorter measures the same, as the timer
 * slack sleeps about 50 µs anyway. */
#define READ_PAUSE_US 50
/* Without a pidfd the child is asked for this often; the shell's end is seen
 * through the pty's too, but jobs can keep that open. */
#define CHILD_POLL_MS 250
/* the shell's time to end after SIGHUP when the window closes (CAVEATS.md) */
#define HANGUP_GRACE_MS 2000
/* key repeat until the compositor says (wl_keyboard.repeat_info) */
#define REPEAT_RATE_DEFAULT 25  /* per second */
#define REPEAT_DELAY_DEFAULT 600 /* ms */
/* the mouse pointer's size without XCURSOR_SIZE, and the most it may ask for: no
 * theme has more, and the size times the scale stays an int */
#define CURSOR_SIZE_DEFAULT 24
#define CURSOR_SIZE_MAX 256

/* The raw paste is done with: up to PASTE_MAX, it is not kept for the next. */
static void paste_free(void)
{
	free(t.paste_buf);
	t.paste_buf = NULL;
	t.paste_len = t.paste_cap = 0;
}

static void pty_write(const char *s, size_t n)
{
	if (!out_write(&t.out, t.pty, s, n))
		fprintf(stderr, "mwt: %zu bytes to the program dropped: out of memory\n", n);
}

static void pty_puts(const char *s)
{
	pty_write(s, strlen(s));
}

/* user input: snap the view back to the live screen */
static void send_input(const char *s, size_t n)
{
	if (t.vt->view) {
		vt_scroll_view(t.vt, -t.vt->view);
		t.need_redraw = true;
	}
	pty_write(s, n);
}

/* A key that raises a signal must not wait behind a paste the program does not
 * read: the rest of the paste goes first (out_drop). */
static void drop_paste_for_signal(unsigned char c)
{
	if (!shell_signal_drops(&t.out, t.paste_fd >= 0, t.pty, c))
		return;
	if (t.paste_fd >= 0) {
		close(t.paste_fd);
		t.paste_fd = -1;
		paste_free();
	}
	out_drop(&t.out, t.pty);
}

static void vt_reply(void *ud, const char *s, size_t n)
{
	(void)ud;
	if (!out_reply(&t.out, t.pty, s, n, REPLY_BACKLOG_MAX))
		fprintf(stderr, "mwt: %zu bytes to the program dropped: out of memory\n", n);
}

/* A token without a serial: sway and mutter mark the window urgent rather
 * than focus it. KWin focuses it when no window is active, or when its focus
 * stealing prevention is off (CAVEATS.md). */
static void bell_token_done(void *d, struct xdg_activation_token_v1 *tok, const char *token)
{
	(void)d;
	xdg_activation_v1_activate(t.activation, token, t.surface);
	xdg_activation_token_v1_destroy(tok);
}
static const struct xdg_activation_token_v1_listener bell_token_listener = { bell_token_done };

/* BEL asks for attention while the window has no focus, once until it gets it
 * back: a program ringing in a loop sends no more. Not before the window shows:
 * compositors take a token then as where to start it (sway: the workspace
 * focused now). Not while mwt's menu is open, where the compositor gives the
 * menu the keyboard (mutter, KWin; sway keeps it on the window). tests/bell.py
 * covers neither of these two: a BEL that early is a race, and sway keeps the
 * focus. */
static void vt_bell(void *ud)
{
	(void)ud;
	if (t.focused || t.bell_asked || !t.activation || !t.last || t.menu.popup)
		return;
	t.bell_asked = true;
	struct xdg_activation_token_v1 *tok = xdg_activation_v1_get_activation_token(t.activation);
	xdg_activation_token_v1_add_listener(tok, &bell_token_listener, NULL);
	xdg_activation_token_v1_set_surface(tok, t.surface);
	xdg_activation_token_v1_commit(tok);
}

/* Only noted: a program may set thousands in one read, and each would be a
 * request; the event loop sends the last one once a turn (send_title). */
static void vt_title(void *ud, const char *s)
{
	(void)ud;
	snprintf(t.title, sizeof t.title, "%s", s);
	t.title_new = true;
}

/* true when it sent one. Called only once the compositor has read all mwt sent
 * before: a compositor that falls behind would otherwise get one more each turn,
 * and libwayland before 1.23 ends the connection when its buffer is full. */
static bool send_title(void)
{
	bool sent = t.title_new && t.toplevel;
	if (sent)
		xdg_toplevel_set_title(t.toplevel, t.title);
	t.title_new = false;
	return sent;
}

/* ---- selection -------------------------------------------------------- */

static char *selection_text(void)
{
	char *s = sel_text(t.vt, &t.sel);
	if (!s && t.sel.active)
		fputs("mwt: no memory for the selection\n", stderr);
	return s;
}

static void sel_clear(void)
{
	if (t.sel.active || t.sel.dragging) {
		sel_mark(t.vt, &t.sel);
		t.sel.active = t.sel.dragging = false;
		t.need_redraw = true;
	}
}

/* ---- clipboard and primary selection ---------------------------------- */

/* slots stay in age order, oldest first */
static void xfer_drop(int i)
{
	close(t.xfers[i].fd);
	free(t.xfers[i].data);
	t.nxfers--;
	memmove(&t.xfers[i], &t.xfers[i + 1], (t.nxfers - i) * sizeof t.xfers[0]);
}

/* write what the pipe takes now; false once the transfer is over */
static bool xfer_step(struct xfer *x)
{
	while (x->off < x->len) {
		ssize_t n = write(x->fd, x->data + x->off, x->len - x->off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0)
			return errno == EAGAIN; /* EPIPE: the reader went away */
		x->off += n;
	}
	return false;
}

static void xfers_run(void)
{
	for (int i = t.nxfers - 1; i >= 0; i--)
		if (!xfer_step(&t.xfers[i]))
			xfer_drop(i);
}

/*
 * Never write to a reader's pipe blocking: a reader that stops reading would
 * stop mwt with it. Each reader gets its own copy, so a new selection does not
 * change a transfer under way, and the rest goes out from the event loop. When
 * all slots hang on readers that do not read, the oldest is cut off.
 */
static void send_text(const char *text, const char *mime, int fd)
{
	char *data = text && !strcmp(mime, "STRING") ? to_latin1(text) :
		     strdup(text ? text : "");
	if (!data || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
		free(data);
		close(fd);
		return;
	}
	if (t.nxfers == MAX_XFERS)
		xfer_drop(0);
	struct xfer *x = &t.xfers[t.nxfers++];
	*x = (struct xfer){ fd, data, strlen(data), 0 };
	if (!xfer_step(x))
		xfer_drop(t.nxfers - 1);
}

/* an offer's rank before any of its types is known, and of a type not in mimes */
#define MIME_NONE MIME_COUNT

static void ds_target(void *d, struct wl_data_source *s, const char *m) { (void)d; (void)s; (void)m; }
static void ds_send(void *d, struct wl_data_source *s, const char *m, int fd)
{
	(void)d; (void)s;
	send_text(t.clip_text, m, fd);
}
static void ds_cancelled(void *d, struct wl_data_source *s)
{
	(void)d;
	if (s == t.clip_src)
		t.clip_src = NULL;
	wl_data_source_destroy(s);
}
static void ds_noop(void *d, struct wl_data_source *s) { (void)d; (void)s; }
static void ds_action(void *d, struct wl_data_source *s, uint32_t a) { (void)d; (void)s; (void)a; }
static const struct wl_data_source_listener ds_listener = {
	ds_target, ds_send, ds_cancelled, ds_noop, ds_noop, ds_action,
};

static void ps_send(void *d, struct zwp_primary_selection_source_v1 *s, const char *m, int fd)
{
	(void)d; (void)s;
	send_text(t.prim_text, m, fd);
}
static void ps_cancelled(void *d, struct zwp_primary_selection_source_v1 *s)
{
	(void)d;
	if (s == t.prim_src)
		t.prim_src = NULL;
	zwp_primary_selection_source_v1_destroy(s);
}
static const struct zwp_primary_selection_source_v1_listener ps_listener = {
	ps_send, ps_cancelled,
};

static void copy_clipboard(void)
{
	char *s = selection_text();
	if (!s || !*s || !t.ddev) {
		free(s);
		return;
	}
	free(t.clip_text);
	t.clip_text = s;
	/* The new source first: out of memory the old one stays, and as it sends
	 * clip_text, the clipboard has the new text all the same. */
	struct wl_data_source *src = wl_data_device_manager_create_data_source(t.ddm);
	if (!src)
		return;
	if (t.clip_src)
		wl_data_source_destroy(t.clip_src);
	t.clip_src = src;
	for (size_t i = 0; i < LEN(mimes); i++)
		wl_data_source_offer(t.clip_src, mimes[i]);
	wl_data_source_add_listener(t.clip_src, &ds_listener, NULL);
	wl_data_device_set_selection(t.ddev, t.clip_src, t.serial);
}

static void copy_primary(void)
{
	char *s = selection_text();
	if (!s || !*s || !t.pdev) {
		free(s);
		return;
	}
	free(t.prim_text);
	t.prim_text = s;
	/* as copy_clipboard: the old source stays when there is no new one */
	struct zwp_primary_selection_source_v1 *src =
		zwp_primary_selection_device_manager_v1_create_source(t.psm);
	if (!src)
		return;
	if (t.prim_src)
		zwp_primary_selection_source_v1_destroy(t.prim_src);
	t.prim_src = src;
	for (size_t i = 0; i < LEN(mimes); i++)
		zwp_primary_selection_source_v1_offer(t.prim_src, mimes[i]);
	zwp_primary_selection_source_v1_add_listener(t.prim_src, &ps_listener, NULL);
	zwp_primary_selection_device_v1_set_selection(t.pdev, t.prim_src, t.serial);
}

/* Everything down to the last row with text: the scrollback and the screen, on
 * the alternate screen only that. Goes to the primary selection like a drag. */
static void select_all(void)
{
	struct sel old = t.sel;
	if (!sel_all(t.vt, &t.sel))
		return;
	sel_mark(t.vt, &old);
	sel_mark(t.vt, &t.sel);
	t.need_redraw = true;
	copy_primary();
}

static void paste_text(const char *s, size_t n)
{
	bool br = t.vt->mode & MODE_BRACKETED;
	/* Counted as UTF-8, which Latin-1 doubles, and with what still waits for the
	 * program: pasting again into one that does not read would grow without end.
	 * mwt's own selection comes here without passing read_paste. */
	if (n > PASTE_MAX || t.out.len + n > PASTE_MAX) {
		if (n > PASTE_MAX)
			fprintf(stderr, "mwt: paste dropped: larger than %d MiB\n", PASTE_MAX_MIB);
		else
			fprintf(stderr, "mwt: paste dropped: %d MiB would wait for the program\n",
				PASTE_MAX_MIB);
		return;
	}
	char *buf = malloc(n + 12);
	if (!buf)
		return;
	send_input(buf, paste_filter(s, n, br, buf));
	free(buf);
}

/* offers carry their best mime rank as user data */
static void offer_offer(void *d, struct wl_data_offer *o, const char *mime)
{
	(void)o;
	int *rank = d;
	*rank = MIN(*rank, mime_rank(mime));
}
static void offer_actions(void *d, struct wl_data_offer *o, uint32_t a) { (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = {
	offer_offer, offer_actions, offer_actions,
};

static void destroy_offer(struct wl_data_offer *o)
{
	if (!o)
		return;
	free(wl_data_offer_get_user_data(o));
	wl_data_offer_destroy(o);
}

static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)d; (void)dd;
	/* out of memory the offer gets no rank and no listener: its events go
	 * unheard, and a paste from it does nothing (paste) */
	int *rank = malloc(sizeof *rank);
	if (!rank)
		return;
	*rank = MIME_NONE;
	wl_data_offer_add_listener(o, &offer_listener, rank);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t serial,
		     struct wl_surface *s, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o)
{
	(void)d; (void)dd; (void)serial; (void)s; (void)x; (void)y;
	destroy_offer(t.dnd_offer);
	t.dnd_offer = o; /* drag and drop is not supported */
}
static void dd_leave(void *d, struct wl_data_device *dd)
{
	(void)d; (void)dd;
	destroy_offer(t.dnd_offer);
	t.dnd_offer = NULL;
}
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t tm, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)dd; (void)tm; (void)x; (void)y;
}
static void dd_drop(void *d, struct wl_data_device *dd) { (void)d; (void)dd; }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	(void)d; (void)dd;
	destroy_offer(t.clip_offer);
	t.clip_offer = o;
}
static const struct wl_data_device_listener dd_listener = {
	dd_data_offer, dd_enter, dd_leave, dd_motion, dd_drop, dd_selection,
};

static void poffer_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *mime)
{
	(void)o;
	int *rank = d;
	*rank = MIN(*rank, mime_rank(mime));
}
static const struct zwp_primary_selection_offer_v1_listener poffer_listener = {
	poffer_offer,
};

static void pd_data_offer(void *d, struct zwp_primary_selection_device_v1 *pd,
			  struct zwp_primary_selection_offer_v1 *o)
{
	(void)d; (void)pd;
	int *rank = malloc(sizeof *rank); /* out of memory as in dd_data_offer */
	if (!rank)
		return;
	*rank = MIME_NONE;
	zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, rank);
}
static void pd_selection(void *d, struct zwp_primary_selection_device_v1 *pd,
			 struct zwp_primary_selection_offer_v1 *o)
{
	(void)d; (void)pd;
	if (t.prim_offer) {
		free(zwp_primary_selection_offer_v1_get_user_data(t.prim_offer));
		zwp_primary_selection_offer_v1_destroy(t.prim_offer);
	}
	t.prim_offer = o;
}
static const struct zwp_primary_selection_device_v1_listener pd_listener = {
	pd_data_offer, pd_selection,
};

static void paste(bool primary)
{
	if (primary && t.prim_src && t.prim_text) {
		paste_text(t.prim_text, strlen(t.prim_text));
		return;
	}
	if (!primary && t.clip_src && t.clip_text) {
		paste_text(t.clip_text, strlen(t.clip_text));
		return;
	}
	int *rank = primary ?
		(t.prim_offer ? zwp_primary_selection_offer_v1_get_user_data(t.prim_offer) : NULL) :
		(t.clip_offer ? wl_data_offer_get_user_data(t.clip_offer) : NULL);
	if (!rank || *rank >= (int)LEN(mimes))
		return;
	/* Only our end: a source that writes in a plain loop would stop at a full pipe,
	 * and the paste would end there without a word. */
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return;
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	if (primary)
		zwp_primary_selection_offer_v1_receive(t.prim_offer, mimes[*rank], fds[1]);
	else
		wl_data_offer_receive(t.clip_offer, mimes[*rank], fds[1]);
	close(fds[1]);
	if (t.paste_fd >= 0)
		close(t.paste_fd);
	t.paste_fd = fds[0];
	t.paste_len = 0;
	t.paste_latin1 = !strcmp(mimes[*rank], "STRING");
	wl_display_flush(t.display);
}

/* A source may never stop writing: the rest waits for the next turn of the event
 * loop, so keys still get through, and past PASTE_MAX the paste goes whole. */
static void read_paste(void)
{
	int r = paste_read(t.paste_fd, &t.paste_buf, &t.paste_len, &t.paste_cap, PASTE_MAX,
			   PASTE_TURN_MAX);
	if (r == PASTE_MORE)
		return;
	close(t.paste_fd);
	t.paste_fd = -1;
	char *buf = NULL;
	size_t len;
	if (r == PASTE_OK)
		r = paste_decode(t.paste_buf, t.paste_len, t.paste_latin1, PASTE_MAX, &buf, &len);
	paste_free();
	if (r == PASTE_NOMEM) {
		fprintf(stderr, "mwt: paste dropped: out of memory\n");
		return;
	}
	if (r == PASTE_TOO_LARGE) {
		fprintf(stderr, "mwt: paste dropped: larger than %d MiB\n", PASTE_MAX_MIB);
		return;
	}
	paste_text(buf ? buf : "", len);
	free(buf);
}

/* ---- rendering -------------------------------------------------------- */

/* config.h, then what mwtrc says for every window and for this app_id. */
static void load_colors(const char *app_id)
{
	t.col = (struct rc_colors){ .fg = color_fg, .bg = color_bg,
		.cursor = color_cursor, .sel_fg = color_sel_fg, .sel_bg = color_sel_bg,
		.min_contrast = min_contrast };
	memcpy(t.col.palette, colors16, sizeof(t.col.palette));
	snprintf(t.col.launcher, sizeof t.col.launcher, "%s", url_launcher);
	char path[4096];
	if (!rc_path(path, sizeof(path)))
		return;
	FILE *f = fopen(path, "r");
	if (!f) {
		if (errno != ENOENT)
			fprintf(stderr, "mwt: %s: %s\n", path, strerror(errno));
		return;
	}
	rc_read(f, path, app_id, &t.col);
	fclose(f);
}

static void init_palette(void)
{
	for (int i = 0; i < 16; i++)
		t.palette[i] = t.col.palette[i];
	for (int i = 0; i < 216; i++) {
		static const uint8_t lv[6] = { 0, 95, 135, 175, 215, 255 };
		t.palette[16 + i] = lv[i / 36] << 16 | lv[i / 6 % 6] << 8 | lv[i % 6];
	}
	for (int i = 0; i < 24; i++) {
		uint32_t v = 8 + i * 10;
		t.palette[232 + i] = v << 16 | v << 8 | v;
	}
}

static uint32_t rgb(uint32_t c)
{
	if (COL_IS_RGB(c))
		return c & 0xffffff;
	if (c == COL_DEFAULT_FG)
		return t.col.fg;
	if (c == COL_DEFAULT_BG)
		return t.col.bg;
	return t.palette[c & 0xff];
}

/* What OSC 4, 10, 11 and 12 queries answer: the colors as drawn. */
static uint32_t vt_color(void *ud, uint32_t c)
{
	(void)ud;
	return c == COL_CURSOR ? t.col.cursor : rgb(c);
}

/* Red and blue in one word, 16 bits each, and green on its own. A lane holds at
 * most 255 * 255, and for those (x + 1 + (x >> 8)) >> 8 is exactly x / 255, so this
 * gives the same pixels as dividing each channel, which tests/pixel.py relies on. */
static inline uint32_t blend(uint32_t bg, uint32_t fg, unsigned a)
{
	uint32_t rb = (fg & 0xff00ff) * a + (bg & 0xff00ff) * (255 - a);
	uint32_t g = (fg & 0xff00) * a + (bg & 0xff00) * (255 - a);
	rb = (rb + 0x10001 + (rb >> 8 & 0xff00ff)) >> 8 & 0xff00ff;
	g = (g + 0x100 + (g >> 8 & 0xffff00)) >> 8 & 0xff00;
	return 0xff000000 | rb | g;
}

/* rc_readable for one cell; neighbors mostly share their colors, and the
 * colors do not change after startup, so the last answer is kept. */
static uint32_t readable(uint32_t fg, uint32_t bg)
{
	static uint32_t last_fg, last_bg, last;
	static bool have;
	if (!have || fg != last_fg || bg != last_bg) {
		last_fg = fg;
		last_bg = bg;
		last = rc_readable(fg, bg, &t.col);
		have = true;
	}
	return last;
}

static void fill_rect(struct buffer *b, int x, int y, int w, int h, uint32_t c)
{
	int x1 = MIN(x + w, b->w), y1 = MIN(y + h, b->h);
	x = MAX(x, 0);
	y = MAX(y, 0);
	if (x >= x1 || y >= y1)
		return;
	/* one row by hand, the others copied: memcpy has the wide stores */
	uint32_t *first = b->data + (size_t)y * b->w;
	c |= 0xff000000;
	for (int xx = x; xx < x1; xx++)
		first[xx] = c;
	for (int yy = y + 1; yy < y1; yy++)
		memcpy(b->data + (size_t)yy * b->w + x, first + x, (x1 - x) * sizeof *first);
}

static void blit(struct buffer *b, const struct glyph *g, int x, int y,
		 int cx0, int cx1, int cy0, int cy1, uint32_t fg)
{
	/* the clip once, in glyph coordinates, and not at every pixel */
	int ox = x + g->left, oy = y - g->top;
	int gx0 = MAX(0, MAX(cx0, 0) - ox), gx1 = MIN(g->w, MIN(cx1, b->w) - ox);
	int gy0 = MAX(0, MAX(cy0, 0) - oy), gy1 = MIN(g->h, MIN(cy1, b->h) - oy);
	fg |= 0xff000000;
	for (int gy = gy0; gy < gy1; gy++) {
		uint32_t *row = b->data + (size_t)(oy + gy) * b->w;
		const uint8_t *a = g->alpha + gy * g->w;
		for (int gx = gx0; gx < gx1; gx++) {
			unsigned v = a[gx];
			if (v == 255)
				row[ox + gx] = fg;
			else if (v)
				row[ox + gx] = blend(row[ox + gx], fg, v);
		}
	}
}

static bool cursor_shown(void)
{
	return !(t.vt->mode & MODE_HIDECURSOR) && t.vt->view == 0;
}

static void draw_row(struct buffer *b, int r, bool sel, int64_t ba, int bc,
		     int64_t ea, int ec)
{
	struct vt *vt = t.vt;
	int len;
	const struct cell *l = vt_view_line(vt, r, &len);
	int64_t abs = (int64_t)vt->scrolled - vt->view + r;
	int y0 = t.pad + r * t.ch;
	bool cur_row = cursor_shown() && r == vt->c.y;
	static const struct cell blankc = { ' ', COL_DEFAULT_FG, COL_DEFAULT_BG, 0 };

	/* Backgrounds first, a run of cells of one color at a time, then what goes on
	 * top. Each of those stays inside its own cell, so the order does not show. */
	int run_x = 0, run_w = 0;
	uint32_t run_bg = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int col = 0; col < vt->cols; col++) {
			struct cell c = l && col < len ? l[col] : blankc;
			if ((c.attr & ATTR_WDUMMY) && col > 0 && l && col - 1 < len &&
			    (l[col - 1].attr & ATTR_WIDE))
				continue; /* drawn together with its left half */
			if (c.attr & ATTR_WDUMMY)
				c = blankc;
			int span = (c.attr & ATTR_WIDE) && col + 1 < vt->cols ? 2 : 1;
			int x0 = t.pad + col * t.cw;
			bool cur = cur_row && (vt->c.x == col || (span == 2 && vt->c.x == col + 1));
			/* a blank's colors only for its background: no readable() for it,
			 * which would evict the one entry between two words */
			bool linked = t.link_on && sel_covers(t.link.ba, t.link.bc, t.link.ea,
							       t.link.ec, abs, col, span);
			if (linked)
				c.attr |= ATTR_UNDERLINE;
			bool ink = !(c.attr & ATTR_INVISIBLE) &&
				   (c.cp > ' ' || (c.attr & (ATTR_UNDERLINE | ATTR_STRIKE)));
			if (pass == 1 && !ink && !cur) {
				col += span - 1;
				continue;
			}
			uint32_t fg = rgb(c.fg), bg = rgb(c.bg);
			if (c.attr & ATTR_REVERSE) {
				uint32_t tmp = fg; fg = bg; bg = tmp;
			}
			bool selected = sel && sel_covers(ba, bc, ea, ec, abs, col, span);
			if (selected) {
				fg = t.col.sel_fg;
				bg = t.col.sel_bg;
			}
			/* one that stands out from the cell: on a reverse cell the
			 * foreground color would vanish */
			uint32_t cc = cur ? rc_cursor(t.col.cursor, fg, bg, &t.col) : 0;
			bool block = cur && t.focused && vt->cursor_style == CURSOR_BLOCK;
			if (block) {
				fg = bg;
				bg = cc;
			}
			if (pass == 1 && ink) { /* pass 0 needs only bg; the selection's are kept */
				/* blocks as they are: in pixel art from ▀ and ▄ both colors
				 * are pixels, and a readable one would be a wrong one */
				if ((!selected && !box_block(c.cp)) || block)
					fg = readable(fg, bg);
				if ((c.attr & ATTR_DIM) && !selected && !block)
					fg = blend(bg, fg, DIM_ALPHA) & 0xffffff;
			}
			if (pass == 0) {
				if (run_w && run_bg == bg && run_x + run_w == x0) {
					run_w += span * t.cw;
				} else {
					if (run_w)
						fill_rect(b, run_x, y0, run_w, t.ch, run_bg);
					run_x = x0;
					run_w = span * t.cw;
					run_bg = bg;
				}
				col += span - 1;
				continue;
			}
			if (c.cp > ' ' && !(c.attr & ATTR_INVISIBLE)) {
				int style = (c.attr & ATTR_BOLD ? 1 : 0) | (c.attr & ATTR_ITALIC ? 2 : 0);
				const struct glyph *g = font_glyph(t.font, c.cp, style, span == 2);
				/* A cell beyond its own either way, in the grid: an
				 * italic or a bold leans into the next, and as rows
				 * are drawn whole, what it covers there is drawn again
				 * with it. No more: a glyph far too wide would cover
				 * the text after it; font.c fits the fallback ones. */
				if (g->w)
					blit(b, g, x0, y0 + t.ascent, MAX(t.pad, x0 - t.cw),
					     MIN(t.pad + vt->cols * t.cw, x0 + (span + 1) * t.cw),
					     y0, y0 + t.ch, fg);
			}
			int thick = MAX(1, t.scale);
			/* invisible hides the lines too, as xterm: hidden input fields */
			/* below the baseline, but in the row at the smallest sizes */
			if ((c.attr & (ATTR_UNDERLINE | ATTR_INVISIBLE)) == ATTR_UNDERLINE)
				fill_rect(b, x0, MIN(y0 + t.ascent + thick, y0 + t.ch - thick),
					  span * t.cw, thick, fg);
			if ((c.attr & (ATTR_STRIKE | ATTR_INVISIBLE)) == ATTR_STRIKE)
				fill_rect(b, x0, y0 + t.ascent * 2 / 3, span * t.cw, thick, fg);
			if (cur && !block) {
				int w = span * t.cw;
				if (!t.focused) {
					fill_rect(b, x0, y0, w, thick, cc);
					fill_rect(b, x0, y0 + t.ch - thick, w, thick, cc);
					fill_rect(b, x0, y0, thick, t.ch, cc);
					fill_rect(b, x0 + w - thick, y0, thick, t.ch, cc);
				} else if (vt->cursor_style == CURSOR_UNDERLINE) {
					fill_rect(b, x0, y0 + t.ch - 2 * thick, w, 2 * thick, cc);
				} else {
					fill_rect(b, x0, y0, 2 * thick, t.ch, cc);
				}
			}
			col += span - 1;
		}
		if (pass == 0 && run_w)
			fill_rect(b, run_x, y0, run_w, t.ch, run_bg);
	}
}

static void buffer_release(void *d, struct wl_buffer *wb)
{
	(void)wb;
	((struct buffer *)d)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

/* wl_shm pool sizes are int32 */
static bool buffer_fits(int w, int h)
{
	return (size_t)w * h * 4 <= INT32_MAX;
}

/* Once until a buffer is made again: a failing one fails each frame. */
static bool buffer_told;
static void buffer_failed(int w, int h, const char *what)
{
	if (!buffer_told)
		fprintf(stderr, "mwt: no %dx%d buffer: %s: %s\n", w, h, what, strerror(errno));
	buffer_told = true;
}

static bool buffer_init(struct buffer *b, int w, int h)
{
	if (!buffer_fits(w, h)) {
		static bool told;
		if (!told)
			fprintf(stderr, "mwt: a %dx%d window needs a buffer over 2 GiB, not drawing\n", w, h);
		told = true;
		return false;
	}
	/* The new one whole first, and the old one goes only then: a failure leaves
	 * b as it was. Freed early, it kept its old size over freed memory, and a
	 * window back at that size drew into it. */
	size_t size = (size_t)w * h * 4;
	int fd = memfd_create("mwt", MFD_CLOEXEC);
	if (fd < 0) {
		buffer_failed(w, h, "memfd_create");
		return false;
	}
	void *data = MAP_FAILED;
	struct wl_buffer *wb = NULL;
	const char *what = "ftruncate";
	if (ftruncate(fd, (off_t)size) == 0) {
		what = "mmap";
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}
	if (data != MAP_FAILED) {
		what = "wl_shm";
		struct wl_shm_pool *pool = wl_shm_create_pool(t.shm, fd, (int32_t)size);
		if (pool) {
			wb = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
			wl_shm_pool_destroy(pool);
		}
	}
	if (!wb)
		buffer_failed(w, h, what);
	close(fd);
	if (!wb) {
		if (data != MAP_FAILED)
			munmap(data, size);
		return false;
	}
	buffer_told = false;
	if (b->wb) {
		wl_buffer_destroy(b->wb);
		munmap(b->data, b->size);
	}
	wl_buffer_add_listener(wb, &buffer_listener, b);
	*b = (struct buffer){ .wb = wb, .data = data, .w = w, .h = h, .size = size };
	return true;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
	(void)d; (void)time;
	wl_callback_destroy(cb);
	t.frame_cb = NULL;
}
static const struct wl_callback_listener frame_listener = { frame_done };

/* A compositor may hold on to the buffer it shows, so frames alternate between two,
 * and each must start as the last one ended. Most frames change little, and the
 * whole picture is megabytes: only the rows that differ get copied. Of a cell row
 * this frame draws anew, only what lies beside the grid. Without the memory to keep
 * track, all of it. */
static void buffer_catch_up(struct buffer *to, const struct buffer *from)
{
	if (t.differ_h != to->h) {
		memcpy(to->data, from->data, to->size);
		return;
	}
	const struct vt *vt = t.vt;
	int gx0 = MIN(t.pad, to->w), gx1 = MIN(t.pad + vt->cols * t.cw, to->w);
	for (int y = 0; y < to->h; y++) {
		if (!t.differ[y])
			continue;
		uint32_t *d = to->data + (size_t)y * to->w;
		const uint32_t *s = from->data + (size_t)y * to->w;
		int r = y >= t.pad ? (y - t.pad) / t.ch : -1;
		if (r >= 0 && r < vt->rows && vt->dirty[r]) {
			memcpy(d, s, gx0 * sizeof *d);
			memcpy(d + gx1, s + gx1, (to->w - gx1) * sizeof *d);
		} else {
			memcpy(d, s, to->w * sizeof *d);
		}
	}
	memset(t.differ, 0, to->h);
}

static void buffers_differ_all(int h)
{
	if (t.differ_h != h) {
		free(t.differ);
		t.differ = malloc(h);
		t.differ_h = t.differ ? h : 0;
	}
	if (t.differ)
		memset(t.differ, 1, h);
}

static void buffers_differ_rows(int y, int n)
{
	int end = MIN(y + n, t.differ_h);
	if (y < end)
		memset(t.differ + y, 1, end - y);
}

#ifdef MWT_CHECK_REDRAW
/* For the tests (make evil): each frame against the whole grid drawn from scratch.
 * The incremental path keeps what an earlier frame drew, copies rows between the
 * buffers and skips rows it is about to draw, and relies on draw_row covering each
 * pixel of its row. A difference is reported on stderr, which the tests read. */
static void check_redraw(const struct buffer *b, int64_t ba, int bc, int64_t ea, int ec)
{
	static struct buffer ref;
	if (ref.size != b->size) {
		free(ref.data);
		ref.data = malloc(b->size);
		ref.size = ref.data ? b->size : 0;
	}
	if (!ref.data)
		return;
	ref.w = b->w;
	ref.h = b->h;
	fill_rect(&ref, 0, 0, b->w, b->h, t.col.bg);
	for (int r = 0; r < t.vt->rows; r++)
		draw_row(&ref, r, t.sel.active, ba, bc, ea, ec);
	for (size_t i = 0; i < b->size / sizeof *b->data; i++)
		if (ref.data[i] != b->data[i]) {
			fprintf(stderr, "mwt: redraw check failed at %zu,%zu\n",
				i % b->w, i / b->w);
			return;
		}
}
#endif

static void draw(void)
{
	struct vt *vt = t.vt;
	int W = t.width * t.scale, H = t.height * t.scale;

	/* the text under the pointer may have changed, or the view */
	link_hover();
	/* first, so that buffer_catch_up knows the rows of the cursor are drawn anew */
	bool shown = cursor_shown();
	if (shown != t.last_shown || vt->c.x != t.last_cx || vt->c.y != t.last_cy) {
		if (t.last_shown && t.last_cy < vt->rows)
			vt->dirty[t.last_cy] = true;
		if (shown)
			vt->dirty[vt->c.y] = true;
	}
	t.last_shown = shown;
	t.last_cx = vt->c.x;
	t.last_cy = vt->c.y;

	/* Nothing new, as for a title alone: no commit. One a configure waits
	 * for still goes out. */
	if (t.last && t.last->w == W && t.last->h == H && !t.need_full && !t.acked &&
	    !memchr(vt->dirty, true, vt->rows)) {
		t.need_redraw = false;
		return;
	}

	struct buffer *b = NULL;
	if (t.last && !t.last->busy)
		b = t.last;
	for (int i = 0; !b && i < 2; i++)
		if (!t.bufs[i].busy)
			b = &t.bufs[i];
	if (!b)
		return; /* both held by the compositor; retry on release */

	bool full = t.need_full;
	if (b->w != W || b->h != H) {
		if (!buffer_init(b, W, H)) {
			/* nothing shown yet: the window would never appear, the
			 * shell would run unseen */
			if (!t.last)
				die("no buffer for the first frame");
			return;
		}
		full = true;
	}
	if (b != t.last) {
		if (t.last && t.last->w == W && t.last->h == H && !full)
			buffer_catch_up(b, t.last);
		else
			full = true;
	}
	if (full) {
		fill_rect(b, 0, 0, W, H, t.col.bg);
		vt_mark_all(vt);
		buffers_differ_all(H);
		t.need_full = false;
	}

	int64_t ba = 0, ea = 0;
	int bc = 0, ec = 0;
	if (t.sel.active)
		sel_range(vt, &t.sel, &ba, &bc, &ea, &ec);
	for (int r = 0; r < vt->rows; r++) {
		if (!vt->dirty[r])
			continue;
		int r1 = r;
		while (r1 + 1 < vt->rows && vt->dirty[r1 + 1])
			r1++;
		for (int i = r; i <= r1; i++) {
			draw_row(b, i, t.sel.active, ba, bc, ea, ec);
			vt->dirty[i] = false;
		}
		buffers_differ_rows(t.pad + r * t.ch, (r1 - r + 1) * t.ch);
		if (!full)
			wl_surface_damage_buffer(t.surface, 0, t.pad + r * t.ch, W,
						 (r1 - r + 1) * t.ch);
		r = r1;
	}
	if (full)
		wl_surface_damage_buffer(t.surface, 0, 0, W, H);

#ifdef MWT_CHECK_REDRAW
	check_redraw(b, ba, bc, ea, ec);
#endif
	wl_surface_set_buffer_scale(t.surface, t.scale);
	wl_surface_attach(t.surface, b->wb, 0, 0);
	if (t.frame_cb) /* drawn for a configure before the last frame was done */
		wl_callback_destroy(t.frame_cb);
	t.frame_cb = wl_surface_frame(t.surface);
	if (t.frame_cb) /* out of memory: no pacing, the next change draws */
		wl_callback_add_listener(t.frame_cb, &frame_listener, NULL);
	wl_surface_commit(t.surface);
	b->busy = true;
	t.last = b;
	t.need_redraw = false;
	t.acked = false;
}

/* ---- context menu ----------------------------------------------------- */

static void paste_clipboard(void)
{
	paste(false);
}

static void link_open(char *url, uint32_t serial);

static void open_menu_link(void)
{
	char *url = t.menu_link;
	t.menu_link = NULL;
	if (url) /* t.serial: the click or key that chose it */
		link_open(url, t.serial);
}

static const struct {
	const char *label, *keys;
	void (*run)(void);
} menu_items[] = {
	{ "Open link", "Ctrl+click", open_menu_link },
	{ "Copy", "Ctrl+Shift+C", copy_clipboard },
	{ "Paste", "Ctrl+Shift+V", paste_clipboard },
	{ "Select all", "Ctrl+Shift+A", select_all },
};
#define MENU_COLS 26

static bool in_menu(void)
{
	return t.menu.surface && t.ptr_surface == t.menu.surface;
}

static bool menu_enabled(int i)
{
	return menu_items[i].run == open_menu_link ? t.menu_link != NULL :
	       menu_items[i].run == copy_clipboard ? t.sel.active :
	       menu_items[i].run == paste_clipboard ? t.clip_offer != NULL : true;
}

static void menu_close(void)
{
	if (!t.menu.popup)
		return;
	if (t.ptr_surface == t.menu.surface)
		t.ptr_surface = NULL;
	xdg_popup_destroy(t.menu.popup);
	xdg_surface_destroy(t.menu.xsurface);
	wl_surface_destroy(t.menu.surface);
	for (int i = 0; i < 2; i++) {
		struct buffer *b = &t.menu.bufs[i];
		if (b->wb) {
			wl_buffer_destroy(b->wb);
			munmap(b->data, b->size);
		}
	}
	memset(&t.menu, 0, sizeof t.menu);
	link_hover();
}

static void menu_text(struct buffer *b, const char *s, int x, int y, uint32_t fg)
{
	for (; *s; s++, x += t.cw) {
		const struct glyph *g = font_glyph(t.font, (unsigned char)*s, STYLE_REGULAR, false);
		if (g->w)
			blit(b, g, x, y, 0, b->w, 0, b->h, fg);
	}
}

/* the buffer free to draw in, or NULL */
static struct buffer *menu_buffer(void)
{
	for (int i = 0; i < 2; i++)
		if (!t.menu.bufs[i].busy)
			return &t.menu.bufs[i];
	return NULL;
}

static void menu_draw(struct buffer *b)
{
	int W = t.menu.w * t.scale, H = t.menu.h * t.scale;
	if (b->w != W || b->h != H) {
		if (!buffer_init(b, W, H)) {
			menu_close();
			return;
		}
	}
	int border = t.scale;
	fill_rect(b, 0, 0, W, H, t.col.sel_bg);
	fill_rect(b, border, border, W - 2 * border, H - 2 * border, t.col.bg);
	for (int i = t.menu.first; i < (int)LEN(menu_items); i++) {
		int y = border + (i - t.menu.first) * t.menu.row_h;
		bool on = menu_enabled(i), hover = i == t.menu.hover;
		uint32_t fg = !on ? blend(t.col.bg, t.col.fg, MENU_DISABLED_ALPHA) & 0xffffff : t.col.fg;
		if (hover) {
			fill_rect(b, border, y, W - 2 * border, t.menu.row_h, t.col.sel_bg);
			fg = t.col.sel_fg;
		}
		int base = y + (t.menu.row_h - t.ch) / 2 + t.ascent;
		menu_text(b, menu_items[i].label, border + t.cw, base, fg);
		int n = (int)strlen(menu_items[i].keys);
		menu_text(b, menu_items[i].keys, W - border - (n + 1) * t.cw, base,
			  hover ? fg : blend(t.col.bg, fg, MENU_SHORTCUT_ALPHA) & 0xffffff);
	}
	wl_surface_set_buffer_scale(t.menu.surface, t.scale);
	wl_surface_attach(t.menu.surface, b->wb, 0, 0);
	wl_surface_damage_buffer(t.menu.surface, 0, 0, W, H);
	wl_surface_commit(t.menu.surface);
	b->busy = true;
	t.menu.dirty = false;
}

static void menu_set_hover(int i)
{
	if (i != t.menu.hover) {
		t.menu.hover = i;
		t.menu.dirty = true;
	}
}

/* the item at y (logical, within the popup) */
static void menu_pointer(double y)
{
	int i = t.menu.first + ((int)(y * t.scale) - t.scale) / t.menu.row_h;
	menu_set_hover(y >= 1 && i < (int)LEN(menu_items) && menu_enabled(i) ? i : -1);
}

static void menu_run(int i)
{
	void (*run)(void) = menu_items[i].run;
	menu_close();
	run();
}

static void menu_button(bool press)
{
	if (press)
		t.menu.pressed = t.menu.hover;
	else if (t.menu.hover >= 0 && t.menu.hover == t.menu.pressed)
		menu_run(t.menu.hover);
}

static void menu_key(xkb_keysym_t sym)
{
	int first = t.menu.first, n = LEN(menu_items) - first, step = 0;
	switch (sym) {
	case XKB_KEY_Escape:
		menu_close();
		return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter:
		if (t.menu.hover >= 0)
			menu_run(t.menu.hover);
		return;
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
		step = -1;
		break;
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
		step = 1;
		break;
	default:
		return;
	}
	unsigned on = 0;
	for (int i = 0; i < n; i++)
		on |= menu_enabled(first + i) ? 1u << i : 0;
	int i = menu_next(t.menu.hover >= 0 ? t.menu.hover - first : -1, n, step, on);
	menu_set_hover(i >= 0 ? first + i : -1);
}

static void menu_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	t.menu.configured = true;
	t.menu.dirty = true;
}
static const struct xdg_surface_listener menu_xs_listener = { menu_configure };

static void popup_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{
	(void)d; (void)p; (void)x; (void)y; (void)w; (void)h;
}
static void popup_done(void *d, struct xdg_popup *p)
{
	(void)d; (void)p;
	menu_close();
}
static void popup_repositioned(void *d, struct xdg_popup *p, uint32_t token)
{
	(void)d; (void)p; (void)token;
}
static const struct xdg_popup_listener popup_listener = {
	popup_configure, popup_done, popup_repositioned,
};

static void stop_repeat(void)
{
	struct itimerspec its = { 0 };
	timerfd_settime(t.repeat_fd, 0, &its, NULL);
	t.repeat_key = 0;
}

/* opens at the pointer; needs the serial of the button press in t.serial */
static void menu_open(void)
{
	if (t.menu.popup || !t.seat)
		return;
	t.menu.row_h = t.ch + 2 * t.pad;
	t.menu.hover = t.menu.pressed = -1;
	/* the URL at the pointer now: the text may move before an item is chosen */
	free(t.menu_link);
	t.menu_link = NULL;
	struct link l;
	link_at(t.vt, (int64_t)t.vt->scrolled - t.vt->view + t.mrow, t.mcol, &l, &t.menu_link);
	t.menu.first = t.menu_link ? 0 : 1;
	t.menu.w = (MENU_COLS * t.cw + 2 * t.scale + t.scale - 1) / t.scale;
	t.menu.h = (((int)LEN(menu_items) - t.menu.first) * t.menu.row_h + 2 * t.scale +
		    t.scale - 1) / t.scale;

	stop_repeat(); /* the menu takes the keys, the release may never come here */
	t.menu.surface = wl_compositor_create_surface(t.compositor);
	t.menu.xsurface = t.menu.surface ?
		xdg_wm_base_get_xdg_surface(t.wm_base, t.menu.surface) : NULL;
	struct xdg_positioner *pos = t.menu.xsurface ?
		xdg_wm_base_create_positioner(t.wm_base) : NULL;
	if (!pos) { /* out of memory: no menu */
		if (t.menu.xsurface)
			xdg_surface_destroy(t.menu.xsurface);
		if (t.menu.surface)
			wl_surface_destroy(t.menu.surface);
		t.menu.xsurface = NULL;
		t.menu.surface = NULL;
		return;
	}
	xdg_surface_add_listener(t.menu.xsurface, &menu_xs_listener, NULL);
	xdg_positioner_set_size(pos, t.menu.w, t.menu.h);
	xdg_positioner_set_anchor_rect(pos, MIN(MAX((int)t.px, 0), t.width - 1),
				       MIN(MAX((int)t.py, 0), t.height - 1), 1, 1);
	xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
	xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
	xdg_positioner_set_constraint_adjustment(pos,
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_Y |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
		XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y);
	t.menu.popup = xdg_surface_get_popup(t.menu.xsurface, t.xsurface, pos);
	xdg_positioner_destroy(pos);
	if (!t.menu.popup) {
		xdg_surface_destroy(t.menu.xsurface);
		wl_surface_destroy(t.menu.surface);
		t.menu.xsurface = NULL;
		t.menu.surface = NULL;
		return;
	}
	xdg_popup_add_listener(t.menu.popup, &popup_listener, NULL);
	xdg_popup_grab(t.menu.popup, t.seat, t.serial);
	wl_surface_commit(t.menu.surface);
	link_hover();
}

/* ---- sizing ----------------------------------------------------------- */

static void snap(int cols, int rows, int *w, int *h)
{
	snap_size(cols, rows, t.cw, t.ch, t.pad, t.scale, w, h);
}

/* One cell at least: a compositor's resize may otherwise make the window smaller
 * than the padding, a grid of 1x1 in no space. Sent when it changes. */
static void send_min_size(void)
{
	int w, h;
	snap(1, 1, &w, &h);
	if (t.toplevel && (w != t.min_w || h != t.min_h)) {
		xdg_toplevel_set_min_size(t.toplevel, w, h);
		t.min_w = w;
		t.min_h = h;
	}
}

static void update_grid(void)
{
	int cols = MIN(MAX(1, (t.width * t.scale - 2 * t.pad) / t.cw), VT_MAX_COLS);
	int rows = MIN(MAX(1, (t.height * t.scale - 2 * t.pad) / t.ch), VT_MAX_ROWS);
	if (cols != t.vt->cols || rows != t.vt->rows) {
		/* out of memory: keep the old grid, the rest of the window stays empty */
		if (!vt_resize(t.vt, cols, rows))
			fprintf(stderr, "mwt: no memory for %dx%d, staying at %dx%d\n",
				cols, rows, t.vt->cols, t.vt->rows);
		sel_clear();
		struct winsize ws = { t.vt->rows, t.vt->cols,
				      MIN(t.width * t.scale, 65535), MIN(t.height * t.scale, 65535) };
		if (t.pty >= 0)
			ioctl(t.pty, TIOCSWINSZ, &ws);
	}
	send_min_size();
	/* No vt_mark_all: a new grid is marked by vt_resize, new cells or a new
	 * window size draw whole (need_full, a new buffer). sway configures at
	 * each focus change, and that redrew every row. */
	t.need_redraw = true;
}

/* XCURSOR_SIZE, at most CURSOR_SIZE_MAX */
static int cursor_size(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	char *end;
	errno = 0;
	long n = s ? strtol(s, &end, 10) : 0;
	if (!s || end == s || *end || n <= 0)
		return CURSOR_SIZE_DEFAULT;
	return errno == ERANGE || n > CURSOR_SIZE_MAX ? CURSOR_SIZE_MAX : (int)n;
}

/* The first image of the first of names the theme has, or NULL. */
static struct wl_cursor_image *cursor_find(struct wl_cursor_theme *theme,
					   const char *const *names, size_t n)
{
	for (size_t i = 0; theme && i < n; i++) {
		struct wl_cursor *c = wl_cursor_theme_get_cursor(theme, names[i]);
		if (c && c->image_count)
			return c->images[0];
	}
	return NULL;
}

/* img on pc's surface, made if need be; false when there is none or no buffer. */
static bool cursor_put(struct pcursor *pc, struct wl_cursor_image *img)
{
	struct wl_buffer *wb = img ? wl_cursor_image_get_buffer(img) : NULL;
	if (!wb)
		return false;
	if (!pc->surface)
		pc->surface = wl_compositor_create_surface(t.compositor);
	if (!pc->surface) /* out of memory: the compositor's pointer, tried again */
		return false;
	/* The theme may not have the size asked for: Adwaita gives 64 for 72 at scale 3.
	 * A buffer scale must divide the buffer's size, or the surface is in error; take
	 * the largest that does, and let a viewport shrink the image the rest. */
	int w = img->width, h = img->height;
	int bscale = t.scale;
	while (bscale > 1 && (w % bscale || h % bscale))
		bscale--;
	int dw = w / bscale, dh = h / bscale;
	if (bscale != t.scale && t.viewporter && !pc->viewport)
		pc->viewport = wp_viewporter_get_viewport(t.viewporter, pc->surface);
	/* without a viewport, out of memory, the largest scale that divides */
	if (bscale != t.scale && pc->viewport) {
		bscale = 1;
		dw = MAX(1, (w + t.scale / 2) / t.scale);
		dh = MAX(1, (h + t.scale / 2) / t.scale);
		wp_viewport_set_destination(pc->viewport, dw, dh);
	} else if (pc->viewport) {
		wp_viewport_destroy(pc->viewport);
		pc->viewport = NULL;
	}
	pc->hx = (int)(img->hotspot_x * dw + w / 2) / w;
	pc->hy = (int)(img->hotspot_y * dh + h / 2) / h;
	wl_surface_set_buffer_scale(pc->surface, bscale);
	wl_surface_attach(pc->surface, wb, 0, 0);
	wl_surface_damage_buffer(pc->surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(pc->surface);
	pc->image = img;
	return true;
}

/* The hand over a link while Ctrl is held, else the text cursor; set only when
 * it changes, or with force after an enter or a new image. */
static void cursor_update(bool force)
{
	if (!t.pointer || !t.ptr_surface)
		return;
	struct pcursor *pc = t.ptr_surface == t.surface && t.link_on && t.hand.image ?
		&t.hand : &t.text;
	if (!pc->image || (pc == t.shown && !force))
		return;
	wl_pointer_set_cursor(t.pointer, t.enter_serial, pc->surface, pc->hx, pc->hy);
	t.shown = pc;
}

/* The new theme is loaded before the old one goes, so the cursor surfaces never
 * hold a buffer of a destroyed theme; if it fails, the old one stays. */
static void load_cursor_theme(void)
{
	int px = (int)MIN((long)cursor_size() * t.scale, 4096);
	struct wl_cursor_theme *theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), px, t.shm);
	static const char *const text[] = { "text", "xterm", "left_ptr", "default" };
	static const char *const hand[] = { "pointer", "hand2", "hand1", "hand" };
	t.cursor_retry = !cursor_put(&t.text, cursor_find(theme, text, LEN(text)));
	if (t.cursor_retry) {
		if (theme)
			wl_cursor_theme_destroy(theme);
		return;
	}
	if (!cursor_put(&t.hand, cursor_find(theme, hand, LEN(hand))))
		t.hand.image = NULL; /* the text cursor over links too */
	/* The hotspot changes with the image; set it now, not on the next enter. */
	cursor_update(true);
	if (t.cursor_theme)
		wl_cursor_theme_destroy(t.cursor_theme);
	t.cursor_theme = theme;
}

static void reload_font(double pt, int scale)
{
	menu_close();
	struct font *f = font_open(t.font_spec, pt, scale);
	if (!f)
		return;
	/* A floating window keeps its grid and grows with the font and the scale:
	 * refuse a zoom or a scale past what a buffer holds, as -g is refused,
	 * rather than stop drawing. At the old scale the compositor scales up. */
	int cw, ch, asc, w, h;
	font_metrics(f, &cw, &ch, &asc);
	snap_size(t.vt->cols, t.vt->rows, cw, ch, padding * scale, scale, &w, &h);
	if (t.configured && !t.cfg_fixed &&
	    !buffer_fits((int)MIN((long long)w * scale, INT_MAX), (int)MIN((long long)h * scale, INT_MAX))) {
		if (scale == t.scale)
			fprintf(stderr, "mwt: %g pt would need a buffer over 2 GiB, staying at %g pt\n",
				font_size_pt(f), t.font_pt);
		else
			fprintf(stderr, "mwt: scale %d would need a buffer over 2 GiB, staying at %d\n",
				scale, t.scale);
		font_close(f);
		return;
	}
	font_close(t.font);
	t.font = f;
	t.font_pt = font_size_pt(f);
	bool rescale = scale != t.scale;
	t.scale = scale;
	font_metrics(f, &t.cw, &t.ch, &t.ascent);
	t.pad = padding * scale;
	t.need_full = true; /* a window that keeps its size keeps old cells beside the grid */
	if (rescale && t.shm)
		load_cursor_theme();
	if (t.configured) {
		if (!t.cfg_fixed) /* keep columns and rows, the window follows */
			snap(t.vt->cols, t.vt->rows, &t.width, &t.height);
		update_grid();
	}
}

static void zoom(int delta)
{
	double pt = delta ? t.font_pt + delta : t.font_pt0;
	if (pt >= zoom_min_pt && pt <= zoom_max_pt)
		reload_font(pt, t.scale);
}

/* ---- keyboard --------------------------------------------------------- */


static void view(int delta)
{
	vt_scroll_view(t.vt, delta);
	t.need_redraw = true;
}

/* Does what input_key asks. A key that raises a signal is one byte. */
static void handle_key(xkb_keycode_t code)
{
	if (!t.xkb_state) {
		if (t.compose)
			xkb_compose_state_reset(t.compose);
		return;
	}
	struct key_out k;
	input_key(t.xkb_state, t.compose, t.vt, code, &k);
	switch (k.act) {
	case DO_NONE: break;
	case DO_SEND:
		if (k.n == 1)
			drop_paste_for_signal((unsigned char)k.buf[0]);
		send_input(k.buf, k.n);
		break;
	case DO_SELECT_ALL: select_all(); break;
	case DO_COPY: copy_clipboard(); break;
	case DO_PASTE: paste(false); break;
	case DO_PASTE_PRIMARY: paste(true); break;
	case DO_ZOOM_IN: zoom(1); break;
	case DO_ZOOM_OUT: zoom(-1); break;
	case DO_ZOOM_RESET: zoom(0); break;
	case DO_VIEW_PAGE_UP: view(t.vt->rows - 1); break;
	case DO_VIEW_PAGE_DOWN: view(-(t.vt->rows - 1)); break;
	case DO_VIEW_TOP: view(t.vt->hist_count); break;
	case DO_VIEW_BOTTOM: view(-t.vt->view); break;
	}
}

static void kb_keymap(void *d, struct wl_keyboard *kb, uint32_t fmt, int fd, uint32_t size)
{
	(void)d; (void)kb;
	if (fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	/* not past the mapping, should the compositor's NUL be missing */
	struct xkb_keymap *km = xkb_keymap_new_from_buffer(t.xkb, map, strnlen(map, size),
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!km)
		return;
	/* the old pair stays unless the new one is whole: keys keep working */
	struct xkb_state *st = xkb_state_new(km);
	if (!st) {
		xkb_keymap_unref(km);
		return;
	}
	xkb_state_unref(t.xkb_state);
	xkb_keymap_unref(t.keymap);
	t.keymap = km;
	t.xkb_state = st;
}

static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *s, struct wl_array *keys)
{
	(void)d; (void)kb; (void)keys;
	t.serial = serial;
	if (s && s == t.menu.surface)
		return;
	t.focused = true;
	t.bell_asked = false;
	t.vt->dirty[t.vt->c.y] = true;
	t.need_redraw = true;
	if (t.vt->mode & MODE_FOCUS)
		pty_puts("\033[I");
}

static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)kb; (void)serial;
	stop_repeat();
	if (t.compose)
		xkb_compose_state_reset(t.compose);
	/* Shift let go elsewhere would still count, for Shift+wheel or a click */
	input_leave(t.xkb_state);
	link_hover();
	/* the menu going away reports a leave too, without a surface */
	if ((s && s == t.menu.surface) || !t.focused)
		return;
	t.focused = false;
	t.vt->dirty[t.vt->c.y] = true;
	t.need_redraw = true;
	if (t.vt->mode & MODE_FOCUS)
		pty_puts("\033[O");
}

static void kb_key(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t time,
		   uint32_t key, uint32_t state)
{
	(void)d; (void)kb; (void)time;
	t.serial = serial;
	xkb_keycode_t code = key + 8;
	/* before the menu takes keys: a release while it is open ends the repeat */
	if (state != WL_KEYBOARD_KEY_STATE_PRESSED && code == t.repeat_key)
		stop_repeat();
	if (t.menu.popup) {
		if (state == WL_KEYBOARD_KEY_STATE_PRESSED && t.xkb_state)
			menu_key(xkb_state_key_get_one_sym(t.xkb_state, code));
		return;
	}
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handle_key(code);
		if (t.repeat_rate > 0 && t.keymap && xkb_keymap_key_repeats(t.keymap, code)) {
			t.repeat_key = code;
			long iv = 1000000000L / t.repeat_rate;
			/* a zero it_value disarms the timer: delay 0 would mean no repeat */
			int delay = MAX(t.repeat_delay, 1);
			struct itimerspec its = {
				.it_interval = { iv / 1000000000L, iv % 1000000000L },
				.it_value = { delay / 1000, (delay % 1000) * 1000000L },
			};
			timerfd_settime(t.repeat_fd, 0, &its, NULL);
		}
	}
}

static void kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t dep,
			 uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)d; (void)kb; (void)serial;
	if (t.xkb_state)
		xkb_state_update_mask(t.xkb_state, dep, lat, lock, 0, 0, group);
	link_hover(); /* Ctrl */
}

static void kb_repeat_info(void *d, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
	(void)d; (void)kb;
	t.repeat_rate = rate;
	t.repeat_delay = delay;
}

static const struct wl_keyboard_listener kb_listener = {
	kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat_info,
};

/* ---- pointer ---------------------------------------------------------- */

static bool mouse_mode(void)
{
	return input_mouse_mode(t.vt->mode, input_mods(t.xkb_state));
}

static void mouse_report(int code, bool press)
{
	char buf[40];
	pty_write(buf, input_mouse(t.vt->mode, code, input_mods(t.xkb_state), press,
				   t.mcol, t.mrow, buf, sizeof buf));
}

static void pointer_cell(int *col, int *row)
{
	input_cell(t.px, t.py, t.scale, t.pad, t.cw, t.ch, t.vt->cols, t.vt->rows, col, row);
}

/* The URL under the pointer is underlined and the pointer a hand while Ctrl is
 * held and a click would open it: not while a program has the mouse, nor in a
 * drag, nor with the menu open. */
static void link_hover(void)
{
	struct link l;
	bool on = t.vt && t.ptr_surface && t.ptr_surface == t.surface && !t.menu.popup &&
		  (input_mods(t.xkb_state) & VT_MOD_CTRL) && !t.sel.dragging &&
		  !(t.buttons.held & t.buttons.program) && !mouse_mode() &&
		  link_at(t.vt, (int64_t)t.vt->scrolled - t.vt->view + t.mrow, t.mcol, &l, NULL);
	if (on == t.link_on && (!on || link_same(&l, &t.link)))
		return;
	if (t.link_on)
		sel_mark_lines(t.vt, t.link.ba, t.link.ea);
	if (on) {
		t.link = l;
		sel_mark_lines(t.vt, l.ba, l.ea);
	}
	t.link_on = on;
	t.need_redraw = true;
	cursor_update(false);
}

static void link_launch(const char *url, const char *token)
{
	char buf[sizeof t.col.launcher];
	char *argv[32];
	if (!link_argv(t.col.launcher, url, buf, sizeof buf, argv, LEN(argv)))
		fprintf(stderr, "mwt: urlLauncher \"%s\": too many words\n", t.col.launcher);
	else if (!opener_spawn(argv, token))
		fprintf(stderr, "mwt: cannot start %s: %s\n", argv[0], strerror(errno));
}

static void link_token_done(void *d, struct xdg_activation_token_v1 *tok, const char *token)
{
	link_launch(d, token);
	free(d);
	xdg_activation_token_v1_destroy(tok);
}
static const struct xdg_activation_token_v1_listener link_token_listener = { link_token_done };

/* Opens url, which it frees, with a token for the click or key of serial: the
 * browser may take the focus with it, as the bell's token without a serial
 * may not (CAVEATS.md). Without xdg-activation, without a token. */
static void link_open(char *url, uint32_t serial)
{
	struct xdg_activation_token_v1 *tok = t.activation && t.seat ?
		xdg_activation_v1_get_activation_token(t.activation) : NULL;
	if (!tok) {
		link_launch(url, NULL);
		free(url);
		return;
	}
	xdg_activation_token_v1_add_listener(tok, &link_token_listener, url);
	xdg_activation_token_v1_set_serial(tok, serial, t.seat);
	xdg_activation_token_v1_set_surface(tok, t.surface);
	xdg_activation_token_v1_commit(tok);
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
		      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p;
	t.enter_serial = serial;
	t.ptr_surface = s;
	t.shown = NULL; /* none set for this enter yet */
	if (in_menu()) {
		menu_pointer(wl_fixed_to_double(y));
	} else {
		t.px = wl_fixed_to_double(x);
		t.py = wl_fixed_to_double(y);
		pointer_cell(&t.mcol, &t.mrow);
		link_hover(); /* first: the cursor for it is the one to set */
	}
	if (t.cursor_retry)
		load_cursor_theme(); /* sets the cursor if it works now */
	else
		cursor_update(false);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial; (void)s;
	if (in_menu())
		menu_set_hover(-1);
	t.ptr_surface = NULL;
	link_hover();
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)time;
	if (in_menu()) {
		menu_pointer(wl_fixed_to_double(y));
		return;
	}
	t.px = wl_fixed_to_double(x);
	t.py = wl_fixed_to_double(y);
	int col, row;
	pointer_cell(&col, &row);
	bool moved = input_moved(t.vt->mode, col, row, t.mcol, t.mrow);
	t.mcol = col;
	t.mrow = row;
	link_hover();
	if (t.menu.popup)
		return;

	if (input_move(&t.buttons, mouse_mode())) {
		int code = input_motion(t.vt->mode, t.buttons.held & t.buttons.program, moved);
		if (code >= 0)
			mouse_report(code, true);
		return;
	}
	if (!t.sel.dragging)
		return;
	/* dragging past the top or bottom edge scrolls the history */
	if (t.py * t.scale < t.pad)
		vt_scroll_view(t.vt, 1);
	else if (t.py * t.scale >= t.pad + t.vt->rows * t.ch)
		vt_scroll_view(t.vt, -1);
	int64_t abs = (int64_t)t.vt->scrolled - t.vt->view + row;
	if (sel_drag(t.vt, &t.sel, abs, col))
		t.need_redraw = true;
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		       uint32_t button, uint32_t state)
{
	(void)d; (void)p;
	int code, b = input_button_bit(button, &code);
	if (b < 0)
		return;
	t.serial = serial;
	bool press = state == WL_POINTER_BUTTON_STATE_PRESSED;
	/* a press on the menu goes to neither, nor does its release */
	bool to_program = input_button(&t.buttons, b, press,
				       mouse_mode() && !in_menu() && !t.menu.popup);
	if (to_program) { /* the release of its press too, with the menu open */
		char buf[40];
		pty_write(buf, input_button_report(t.vt->mode, code, input_mods(t.xkb_state),
						   press, t.mcol, t.mrow, buf, sizeof buf));
		return;
	}
	if (b > 2) /* the side buttons are only for a program that has the mouse */
		return;

	unsigned bit = 1u << b;
	if (in_menu() || t.menu.popup) {
		t.buttons.menu = press ? t.buttons.menu | bit : t.buttons.menu & ~bit;
		if (in_menu())
			menu_button(press);
		else if (press) /* on the window, where a grab may let it through */
			menu_close();
		return;
	}
	if (!press && (t.buttons.menu & bit)) { /* the menu's press: not a selection's */
		t.buttons.menu &= ~bit;
		return;
	}
	int64_t abs = (int64_t)t.vt->scrolled - t.vt->view + t.mrow;
	struct link l;
	char *url = NULL;
	if (b == 0 && !press && t.link_pressed) {
		t.link_pressed = false;
	} else if (b == 0 && press && (input_mods(t.xkb_state) & VT_MOD_CTRL) &&
		   link_at(t.vt, abs, t.mcol, &l, &url)) {
		t.link_pressed = true; /* Ctrl+click on a URL opens it, selects nothing */
		link_open(url, serial);
	} else if (b == 0 && press && t.sel.active && (input_mods(t.xkb_state) & VT_MOD_SHIFT)) {
		sel_mark(t.vt, &t.sel);
		sel_extend(&t.sel, abs, t.mcol); /* Shift extends the selection */
		sel_mark(t.vt, &t.sel);
		t.need_redraw = true;
	} else if (b == 0 && press) {
		sel_mark(t.vt, &t.sel);
		sel_press(&t.sel, &t.clicks, time, abs, t.mcol, (uint32_t)double_click_ms);
		sel_mark(t.vt, &t.sel);
		t.need_redraw = true;
	} else if (b == 0) {
		t.sel.dragging = false;
		if (t.sel.active)
			copy_primary();
	} else if (b == 1 && press) {
		paste(true);
	} else if (b == 2 && press) {
		if (t.sel.dragging) {
			t.sel.dragging = false;
			if (t.sel.active)
				copy_primary();
		}
		menu_open();
	}
	link_hover(); /* a drag begun or ended */
}

/* one step of the wheel on axis a, positive down or right */
static void wheel_step(uint32_t a, bool positive)
{
	bool sideways = a == WL_POINTER_AXIS_HORIZONTAL_SCROLL;
	unsigned mods = input_mods(t.xkb_state);
	switch (input_wheel(t.vt->mode, mods, sideways)) {
	case WHEEL_REPORT:
		mouse_report(input_wheel_code(sideways, positive), true);
		if (sideways) /* buttons 6 and 7 come up again, as in xterm; 4 and 5 not */
			mouse_report(input_wheel_code(sideways, positive), false);
		break;
	case WHEEL_KEYS:
		for (int i = 0; i < wheel_lines; i++) {
			char seq[16];
			size_t n = vt_key(t.vt, positive ? VT_KEY_DOWN : VT_KEY_UP, 0, seq, sizeof seq);
			send_input(seq, n);
		}
		break;
	case WHEEL_SCROLL: {
		/* Shift: a page at a time */
		int n = mods & VT_MOD_SHIFT ? t.vt->rows - 1 : wheel_lines;
		vt_scroll_view(t.vt, positive ? -n : n);
		t.need_redraw = true;
		break;
	}
	case WHEEL_NONE:
		break;
	}
}

static void axis_frame(void)
{
	int steps[2];
	input_axis_frame(&t.axis, touchpad_step, steps);
	if (t.menu.popup)
		return;
	for (uint32_t a = 0; a < 2; a++)
		for (int i = steps[a] < 0 ? -steps[a] : steps[a]; i > 0; i--)
			wheel_step(a, steps[a] > 0);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	(void)d; (void)time;
	input_axis_value(&t.axis, axis, wl_fixed_to_double(v));
	/* before version 5 there are no frames: each event is one */
	if (wl_pointer_get_version(p) < WL_POINTER_FRAME_SINCE_VERSION)
		axis_frame();
}

static void ptr_frame(void *d, struct wl_pointer *p)
{
	(void)d; (void)p;
	axis_frame();
}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t tm, uint32_t a)
{
	(void)d; (void)p; (void)tm;
	input_axis_stop(&t.axis, a);
}
/* A wheel's notches, 10 or 15 pixels each in the axis event of the same frame
 * (mutter, wlroots and KWin), which touchpad_step's 10 would make one or 1.5
 * steps: where a frame has notches, they count and the pixels do not
 * (input_axis_frame). */
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t n)
{
	(void)d; (void)p;
	input_axis_discrete(&t.axis, a, n);
}

/* bound at version 5, so the newer events are never sent */
static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
	.button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
	.axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

/* ---- seat, shell, registry -------------------------------------------- */

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !t.keyboard) {
		t.keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(t.keyboard, &kb_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && t.keyboard) {
		/* no leave comes for a keyboard that is gone: the focus goes with it */
		kb_leave(NULL, t.keyboard, t.serial, NULL);
		if (wl_keyboard_get_version(t.keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(t.keyboard);
		else
			wl_keyboard_destroy(t.keyboard);
		t.keyboard = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !t.pointer) {
		t.pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(t.pointer, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && t.pointer) {
		/* nor a release for buttons it held: a program in mouse mode would
		 * wait for one, and a drag would go on with the next pointer */
		static const uint32_t evdev[] = {
			BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA, BTN_FORWARD, BTN_BACK,
		};
		for (size_t b = 0; b < LEN(evdev); b++)
			if (t.buttons.held & 1u << b)
				ptr_button(NULL, t.pointer, t.serial, 0, evdev[b],
					   WL_POINTER_BUTTON_STATE_RELEASED);
		if (wl_pointer_get_version(t.pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(t.pointer);
		else
			wl_pointer_destroy(t.pointer);
		t.pointer = NULL;
		t.ptr_surface = NULL;
	}
}

/* The clipboard and primary selection devices, once there are a seat and managers. */
static void seat_devices(void)
{
	if (t.seat && t.ddm && !t.ddev) {
		t.ddev = wl_data_device_manager_get_data_device(t.ddm, t.seat);
		wl_data_device_add_listener(t.ddev, &dd_listener, NULL);
	}
	if (t.seat && t.psm && !t.pdev) {
		t.pdev = zwp_primary_selection_device_manager_v1_get_device(t.psm, t.seat);
		zwp_primary_selection_device_v1_add_listener(t.pdev, &pd_listener, NULL);
	}
}

/* The seat went away: let go of it and all that hangs on it, so a later one binds. */
static void seat_gone(void)
{
	menu_close();
	seat_caps(NULL, t.seat, 0);
	dd_leave(NULL, NULL);
	dd_selection(NULL, NULL, NULL);
	pd_selection(NULL, NULL, NULL);
	if (t.ddev) {
		if (wl_data_device_get_version(t.ddev) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
			wl_data_device_release(t.ddev);
		else
			wl_data_device_destroy(t.ddev);
		t.ddev = NULL;
	}
	if (t.pdev) {
		zwp_primary_selection_device_v1_destroy(t.pdev);
		t.pdev = NULL;
	}
	if (wl_seat_get_version(t.seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(t.seat);
	else
		wl_seat_destroy(t.seat);
	t.seat = NULL;
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void wm_ping(void *d, struct xdg_wm_base *wm, uint32_t serial)
{
	(void)d;
	xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	(void)d;
	xdg_surface_ack_configure(xs, serial);
	t.acked = true;
	int w = t.cfg_w, h = t.cfg_h;
	if (w <= 0 || h <= 0) {
		if (t.configured) {
			w = t.width;
			h = t.height;
		} else {
			/* -g, but no larger than the compositor says fits */
			int cols = t.init_cols, rows = t.init_rows;
			fit_bounds(&cols, &rows, t.bounds_w, t.bounds_h, t.scale, t.pad, t.cw, t.ch);
			snap(cols, rows, &w, &h);
		}
	} else if (!t.cfg_fixed) {
		int cols = MAX(1, (w * t.scale - 2 * t.pad) / t.cw);
		int rows = MAX(1, (h * t.scale - 2 * t.pad) / t.ch);
		snap(cols, rows, &w, &h);
	}
	t.width = w;
	t.height = h;
	t.configured = true;
	update_grid();
}
static const struct xdg_surface_listener xs_listener = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *tl, int32_t w, int32_t h,
			 struct wl_array *states)
{
	(void)d; (void)tl;
	t.cfg_w = w;
	t.cfg_h = h;
	t.cfg_fixed = false;
	uint32_t *s;
	wl_array_for_each(s, states) {
		switch (*s) {
		case XDG_TOPLEVEL_STATE_MAXIMIZED:
		case XDG_TOPLEVEL_STATE_FULLSCREEN:
		case XDG_TOPLEVEL_STATE_TILED_LEFT:
		case XDG_TOPLEVEL_STATE_TILED_RIGHT:
		case XDG_TOPLEVEL_STATE_TILED_TOP:
		case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
			t.cfg_fixed = true;
			break;
		}
	}
}
static void tl_close(void *d, struct xdg_toplevel *tl)
{
	(void)d; (void)tl;
	t.running = false;
}
static void tl_bounds(void *d, struct xdg_toplevel *tl, int32_t w, int32_t h)
{
	(void)d; (void)tl;
	t.bounds_w = w;
	t.bounds_h = h;
}
static void tl_caps(void *d, struct xdg_toplevel *tl, struct wl_array *c)
{
	(void)d; (void)tl; (void)c;
}
static const struct xdg_toplevel_listener tl_listener = {
	tl_configure, tl_close, tl_bounds, tl_caps,
};

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) { (void)d; (void)s; (void)o; }
static void surf_scale(void *d, struct wl_surface *s, int32_t scale)
{
	(void)d; (void)s;
	if (scale > 0 && scale != t.scale)
		reload_font(t.font_pt, scale);
}
static void surf_transform(void *d, struct wl_surface *s, uint32_t tr) { (void)d; (void)s; (void)tr; }
static const struct wl_surface_listener surf_listener = {
	surf_enter, surf_leave, surf_scale, surf_transform,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		if (ver < 4)
			die("wl_compositor version 4 required");
		t.compositor = wl_registry_bind(r, name, &wl_compositor_interface, MIN(ver, 6));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		t.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		t.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, MIN(ver, 5));
		xdg_wm_base_add_listener(t.wm_base, &wm_listener, NULL);
	} else if (!strcmp(iface, wl_seat_interface.name) && !t.seat) {
		t.seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 5));
		t.seat_name = name;
		wl_seat_add_listener(t.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		t.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
		t.ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, MIN(ver, 3));
	} else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) {
		t.psm = wl_registry_bind(r, name,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	} else if (!strcmp(iface, wp_viewporter_interface.name)) {
		t.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, xdg_activation_v1_interface.name)) {
		t.activation = wl_registry_bind(r, name, &xdg_activation_v1_interface, 1);
	}
	seat_devices();
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r;
	if (t.seat && name == t.seat_name)
		seat_gone();
}
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ---- pty and child ---------------------------------------------------- */

static bool terminfo_in(const char *dir, const char *name)
{
	char path[4096];
	snprintf(path, sizeof path, "%s/%c/%s", dir, name[0], name);
	if (access(path, R_OK) == 0)
		return true;
	snprintf(path, sizeof path, "%s/%02x/%s", dir, (unsigned char)name[0], name);
	return access(path, R_OK) == 0;
}

static bool terminfo_system(const char *name)
{
	static const char *std[] = { "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo",
		"/usr/lib/terminfo" };
	for (size_t i = 0; i < LEN(std); i++)
		if (terminfo_in(std[i], name))
			return true;
	return false;
}

/* Where ncurses looks, in its order: TERMINFO, ~/.terminfo, each of TERMINFO_DIRS,
 * where an empty entry stands for the system's directories, then those. */
static bool have_terminfo(const char *name)
{
	const char *ti = getenv("TERMINFO"), *home = getenv("HOME");
	if (ti && terminfo_in(ti, name))
		return true;
	if (home) {
		char dir[4096];
		snprintf(dir, sizeof dir, "%s/.terminfo", home);
		if (terminfo_in(dir, name))
			return true;
	}
	const char *p = getenv("TERMINFO_DIRS");
	while (p && *p) {
		const char *end = strchr(p, ':');
		size_t len = end ? (size_t)(end - p) : strlen(p);
		char dir[4096];
		if (!len) {
			if (terminfo_system(name))
				return true;
		} else if (len < sizeof dir) {
			memcpy(dir, p, len);
			dir[len] = 0;
			if (terminfo_in(dir, name))
				return true;
		}
		p = end ? end + 1 : NULL;
	}
	return terminfo_system(name);
}

static void spawn(char **argv)
{
	const char *term = have_terminfo(term_name) ? term_name : "xterm-256color";
	if (term != term_name)
		fprintf(stderr, "mwt: no terminfo for '%s', using %s "
			"(run 'tic -x mwt.info')\n", term_name, term);
	if (!child_spawn(&t.child, &t.pty, argv, term, t.vt->cols, t.vt->rows))
		die_errno("cannot start the shell");
}

static void got_output(void *ud, const char *s, size_t n)
{
	(void)ud;
	vt_write(t.vt, s, n);
}

/* Stops after a megabyte or a while, whichever is first, so that a flood leaves room
 * for frames and keys. Short lines scroll at each one, and a megabyte of them takes
 * long: the bytes alone would allow a few frames a second, and Ctrl+C would wait. */
static void read_pty(void)
{
	static char buf[65536];
	struct shell_turn turn = {
		buf, sizeof buf, READ_TURN_MAX, READ_TURN_MS, READ_PAUSE_US,
		wl_display_get_fd(t.display), got_output, NULL
	};
	bool gone = false;
	size_t total = shell_read(t.pty, &turn, &gone);
	if (gone)
		t.running = false; /* the child is gone */
	if (!total)
		return;
	bool alt = t.vt->mode & MODE_ALTSCREEN;
	if (alt != t.was_alt)
		sel_clear();
	t.was_alt = alt;
	t.need_redraw = true;
}

/* ---- main ------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr, "usage: mwt [-v] [-h] [-f font] [-a app-id] [-T title] [-g COLSxROWS] "
		"[-e] [command [args...]]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	stdio_fill(); /* before anything is opened or printed */
	const char *app_id = default_app_id, *title = default_title;
	t.font_spec = font_spec;
	t.init_cols = default_cols;
	t.init_rows = default_rows;
	int opt;
	while ((opt = getopt(argc, argv, "+f:a:T:g:ehv")) != -1) {
		switch (opt) {
		case 'v':
			puts("mwt " MWT_VERSION);
			return 0;
		case 'f': t.font_spec = optarg; break;
		case 'a': app_id = optarg; break;
		case 'T': title = optarg; break;
		case 'g':
			switch (parse_geometry(optarg, VT_MAX_COLS, VT_MAX_ROWS,
					       &t.init_cols, &t.init_rows)) {
			case GEOM_RANGE:
				fprintf(stderr, "mwt: -g takes 1x1 up to %dx%d\n",
					VT_MAX_COLS, VT_MAX_ROWS);
				usage();
				break;
			case GEOM_SYNTAX:
				usage();
				break;
			}
			break;
		case 'e': goto done; /* the rest is the command */
		default: usage();
		}
	}
done:;
	char **cmd = optind < argc ? argv + optind : NULL;

	setlocale(LC_ALL, "");
	if (strcmp(nl_langinfo(CODESET), "UTF-8"))
		setlocale(LC_CTYPE, "C.UTF-8");
	signal(SIGPIPE, SIG_IGN);
	/* SIGCHLD left ignored by what started mwt has the kernel reap the shell:
	 * without a pidfd its end would be seen only once jobs close the pty */
	signal(SIGCHLD, SIG_DFL);
	/* A mask inherited from what started mwt: with SIGTERM blocked, a logout's
	 * SIGTERM would wait for good. What it ignores stays so (nohup). */
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);

	/* One message each: a long one would overflow its size field. */
	static char app_buf[VT_OSC_MAX], title_buf[VT_OSC_MAX];
	snprintf(app_buf, sizeof app_buf, "%.*s", (int)utf8_clip(app_id, sizeof app_buf - 1), app_id);
	snprintf(title_buf, sizeof title_buf, "%.*s", (int)utf8_clip(title, sizeof title_buf - 1), title);
	app_id = app_buf;
	title = title_buf;
	load_colors(app_id);
	init_palette();
	t.font = font_open(t.font_spec, 0, 1);
	if (!t.font)
		die("cannot load font");
	t.font_pt = t.font_pt0 = font_size_pt(t.font);
	font_metrics(t.font, &t.cw, &t.ch, &t.ascent);
	t.pad = padding;
	int init_w, init_h;
	snap(t.init_cols, t.init_rows, &init_w, &init_h);
	if (!buffer_fits(init_w, init_h)) {
		fprintf(stderr, "mwt: -g %dx%d with this font needs a buffer over 2 GiB\n",
			t.init_cols, t.init_rows);
		exit(2);
	}

	struct vt_callbacks cb = { .write = vt_reply, .title = vt_title, .color = vt_color,
				   .bell = vt_bell };
	t.vt = vt_new(t.init_cols, t.init_rows, histsize, &cb);
	if (!t.vt)
		die("out of memory");
	t.vt->allow_checksum = allow_checksum;

	t.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	const char *loc = getenv("LC_ALL");
	if (!loc || !*loc) loc = getenv("LC_CTYPE");
	if (!loc || !*loc) loc = getenv("LANG");
	if (!loc || !*loc) loc = "C";
	struct xkb_compose_table *ct = xkb_compose_table_new_from_locale(t.xkb, loc,
		XKB_COMPOSE_COMPILE_NO_FLAGS);
	if (ct) {
		t.compose = xkb_compose_state_new(ct, XKB_COMPOSE_STATE_NO_FLAGS);
		xkb_compose_table_unref(ct);
	}
	t.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	t.repeat_rate = REPEAT_RATE_DEFAULT;
	t.repeat_delay = REPEAT_DELAY_DEFAULT;

	t.display = wl_display_connect(NULL);
	if (!t.display)
		die_errno("cannot connect to Wayland display");
	t.registry = wl_display_get_registry(t.display);
	wl_registry_add_listener(t.registry, &reg_listener, NULL);
	wl_display_roundtrip(t.display);
	if (!t.compositor || !t.shm || !t.wm_base)
		die("compositor lacks wl_compositor, wl_shm or xdg_wm_base");
	load_cursor_theme();

	t.surface = wl_compositor_create_surface(t.compositor);
	wl_surface_add_listener(t.surface, &surf_listener, NULL);
	t.xsurface = xdg_wm_base_get_xdg_surface(t.wm_base, t.surface);
	xdg_surface_add_listener(t.xsurface, &xs_listener, NULL);
	t.toplevel = xdg_surface_get_toplevel(t.xsurface);
	xdg_toplevel_add_listener(t.toplevel, &tl_listener, NULL);
	xdg_toplevel_set_app_id(t.toplevel, app_id);
	xdg_toplevel_set_title(t.toplevel, title);
	send_min_size();
	if (t.deco_mgr) {
		t.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(t.deco_mgr, t.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(t.deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	/* before the roundtrip: a close in it would be lost */
	t.running = true;
	wl_surface_commit(t.surface);
	wl_display_roundtrip(t.display);

	if (t.running) /* closed already: no command to start and hang up */
		spawn(cmd);
	int wlfd = wl_display_get_fd(t.display);

	while (t.running) {
		/* A hidden window gets no frame callback, but a configure it acked
		 * waits for a commit: some compositors stall until it comes. */
		if (t.need_redraw && t.configured && (!t.frame_cb || t.acked))
			draw();
		if (t.menu.popup && t.menu.configured && t.menu.dirty && menu_buffer())
			menu_draw(menu_buffer());
		while (wl_display_prepare_read(t.display) != 0)
			wl_display_dispatch_pending(t.display);
		bool wl_out = wl_display_flush(t.display) < 0 && errno == EAGAIN;
		if (!wl_out && send_title())
			wl_out = wl_display_flush(t.display) < 0 && errno == EAGAIN;

		struct pollfd fds[5 + MAX_XFERS] = {
			{ wlfd, POLLIN | (wl_out ? POLLOUT : 0), 0 },
			{ t.pty, POLLIN | (t.out.len ? POLLOUT : 0), 0 },
			{ t.repeat_fd, POLLIN, 0 },
			{ t.paste_fd, POLLIN, 0 },
			{ t.child.fd, POLLIN, 0 },
		};
		int nx = t.nxfers;
		for (int i = 0; i < nx; i++)
			fds[5 + i] = (struct pollfd){ t.xfers[i].fd, POLLOUT, 0 };
		/* Without a pidfd (before Linux 5.3, or denied by seccomp) only the
		 * pty's end shows the shell's, and jobs can keep the pty open: the
		 * child is asked for four times a second instead. */
		if (poll(fds, 5 + nx, t.child.fd >= 0 ? -1 : CHILD_POLL_MS) < 0) {
			wl_display_cancel_read(t.display);
			if (errno == EINTR)
				continue;
			break;
		}
		if (t.child.fd < 0 && child_gone(&t.child))
			t.running = false;
		if (fds[0].revents & POLLIN)
			wl_display_read_events(t.display);
		else
			wl_display_cancel_read(t.display);
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(t.display) < 0)
			break;
		/* A frame callback that just came draws before the pty is read: a
		 * turn of reading took up to READ_TURN_MS of the frame, and in a flood
		 * the frame came that much later. */
		if (t.need_redraw && t.configured && !t.frame_cb) {
			draw();
			wl_display_flush(t.display); /* or it waits for the turn too */
		}

		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
			read_pty();
		if (fds[1].revents & POLLOUT)
			out_flush(&t.out, t.pty);
		if (fds[2].revents & POLLIN) {
			uint64_t n = 0;
			if (read(t.repeat_fd, &n, sizeof n) == sizeof n && t.repeat_key)
				for (uint64_t i = 0; i < MIN(n, REPEAT_BURST_MAX); i++)
					handle_key(t.repeat_key);
		}
		if (t.paste_fd >= 0 && (fds[3].revents & (POLLIN | POLLHUP | POLLERR)))
			read_paste();
		if (fds[4].revents & POLLIN)
			t.running = false; /* the pty may stay open, held by jobs */
		/* slots may have moved during dispatch, so try them all */
		for (int i = 0; i < nx; i++)
			if (fds[5 + i].revents) {
				xfers_run();
				break;
			}
	}

	while (t.nxfers)
		xfer_drop(0);
	wl_registry_destroy(t.registry);
	wl_display_disconnect(t.display); /* the window goes first */
	child_hangup(&t.child, &t.pty, HANGUP_GRACE_MS);
	return 0;
}
