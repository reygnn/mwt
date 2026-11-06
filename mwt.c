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
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"

#include "config.h"
#include "font.h"
#include "vt.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LEN(a) (sizeof(a) / sizeof((a)[0]))

struct buffer {
	struct wl_buffer *wb;
	uint32_t *data;
	int w, h;
	size_t size;
	bool busy;
};

enum { SEL_CHAR = 1, SEL_WORD, SEL_LINE };

static const char *mimes[] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

static struct {
	/* globals */
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct wl_pointer *pointer;
	struct zxdg_decoration_manager_v1 *deco_mgr;
	struct wl_data_device_manager *ddm;
	struct wl_data_device *ddev;
	struct zwp_primary_selection_device_manager_v1 *psm;
	struct zwp_primary_selection_device_v1 *pdev;

	/* window */
	struct wl_surface *surface;
	struct xdg_surface *xsurface;
	struct xdg_toplevel *toplevel;
	struct zxdg_toplevel_decoration_v1 *deco;
	struct wl_callback *frame_cb;
	struct buffer bufs[2], *last;
	int width, height; /* logical */
	int scale;
	int cfg_w, cfg_h;
	bool cfg_fixed;    /* maximized, fullscreen or tiled: use size as given */
	bool configured, focused, running, need_redraw;
	int init_cols, init_rows;

	/* font and grid, in buffer pixels */
	struct font *font;
	const char *font_spec;
	double font_pt, font_pt0;
	int cw, ch, ascent, pad;
	uint32_t palette[256];

	/* terminal */
	struct vt *vt;
	int pty;
	pid_t child;
	int child_fd; /* pidfd: the shell can end while jobs keep the pty open */
	char *out;
	size_t out_len, out_cap;
	bool was_alt;
	int last_cx, last_cy;
	bool last_shown;

	/* keyboard */
	struct xkb_context *xkb;
	struct xkb_keymap *keymap;
	struct xkb_state *xkb_state;
	struct xkb_compose_state *compose;
	int repeat_fd, repeat_rate, repeat_delay;
	xkb_keycode_t repeat_key;
	uint32_t serial;

	/* pointer */
	struct wl_cursor_theme *cursor_theme;
	struct wl_cursor_image *cursor_image;
	struct wl_surface *cursor_surface;
	uint32_t enter_serial;
	double px, py;
	int mcol, mrow;
	unsigned buttons;
	double axis_acc;
	uint32_t click_time;
	int click_count, click_col;
	int64_t click_abs;

	/* selection: anchor (aa, ac) and moving end (ea, ec) */
	struct {
		bool active, dragging;
		int mode;
		int64_t aa, ea;
		int ac, ec;
	} sel;
	char *clip_text, *prim_text;
	struct wl_data_source *clip_src;
	struct zwp_primary_selection_source_v1 *prim_src;
	struct wl_data_offer *clip_offer, *dnd_offer;
	struct zwp_primary_selection_offer_v1 *prim_offer;
	int paste_fd;
	char *paste_buf;
	size_t paste_len, paste_cap;
	bool paste_latin1;
} t = { .pty = -1, .repeat_fd = -1, .paste_fd = -1, .child_fd = -1, .scale = 1 };

static void die(const char *msg)
{
	fprintf(stderr, "mwt: %s\n", msg);
	exit(1);
}

/* ---- pty output ------------------------------------------------------- */

static void flush_out(void)
{
	while (t.out_len) {
		ssize_t n = write(t.pty, t.out, t.out_len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				t.out_len = 0;
			return;
		}
		memmove(t.out, t.out + n, t.out_len - n);
		t.out_len -= n;
	}
}

static void pty_write(const char *s, size_t n)
{
	if (t.pty < 0 || !n)
		return;
	if (t.out_len + n > t.out_cap) {
		size_t cap = MAX(t.out_cap * 2, t.out_len + n + 4096);
		char *p = realloc(t.out, cap);
		if (!p)
			return;
		t.out = p;
		t.out_cap = cap;
	}
	memcpy(t.out + t.out_len, s, n);
	t.out_len += n;
	flush_out();
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

/*
 * A key that raises a signal must not wait behind a paste the program does not
 * read, here or in the kernel, whose line buffer takes no more once it is full.
 * So the rest of the paste goes first. TCSAFLUSH on the master flushes the
 * slave's input. A bracket the program has seen open gets closed.
 */
static void drop_paste_for_signal(unsigned char c)
{
	struct termios tm;
	if (t.pty < 0 || !c || tcgetattr(t.pty, &tm) < 0 || !(tm.c_lflag & ISIG) ||
	    (c != tm.c_cc[VINTR] && c != tm.c_cc[VQUIT] && c != tm.c_cc[VSUSP]))
		return;
	if (t.paste_fd >= 0) {
		close(t.paste_fd);
		t.paste_fd = -1;
	}
	if (!t.out_len)
		return;
	bool open_bracket = memmem(t.out, t.out_len, "\033[201~", 6) != NULL;
	t.out_len = 0;
	tcflush(t.pty, TCOFLUSH);
	tcsetattr(t.pty, TCSAFLUSH, &tm);
	if (open_bracket)
		pty_write("\033[201~", 6);
}

static void vt_reply(void *ud, const char *s, size_t n)
{
	(void)ud;
	pty_write(s, n);
}

static void vt_title(void *ud, const char *s)
{
	(void)ud;
	if (t.toplevel)
		xdg_toplevel_set_title(t.toplevel, s);
}

/* ---- selection -------------------------------------------------------- */

static bool is_word(uint32_t cp)
{
	return cp > 0x7f || (cp > ' ' && !strchr("\"'`()[]{}<>|;,", (int)cp));
}

static int cell_class(int64_t abs, int col)
{
	int len;
	const struct cell *l = vt_abs_line(t.vt, abs, &len);
	if (!l || col >= len || l[col].cp == ' ' || l[col].cp == 0)
		return 0;
	return is_word(l[col].cp) ? 1 : 2;
}

/* normalized selection range, inclusive */
static void sel_range(int64_t *ba, int *bc, int64_t *ea, int *ec)
{
	*ba = t.sel.aa; *bc = t.sel.ac;
	*ea = t.sel.ea; *ec = t.sel.ec;
	if (*ea < *ba || (*ea == *ba && *ec < *bc)) {
		int64_t ta = *ba; int tc = *bc;
		*ba = *ea; *bc = *ec;
		*ea = ta; *ec = tc;
	}
	if (t.sel.mode == SEL_LINE) {
		*bc = 0;
		*ec = 1 << 30;
	} else if (t.sel.mode == SEL_WORD) {
		int k = cell_class(*ba, *bc);
		if (k != 2)
			while (*bc > 0 && cell_class(*ba, *bc - 1) == k)
				(*bc)--;
		k = cell_class(*ea, *ec);
		if (k != 2)
			while (*ec < t.vt->cols - 1 && cell_class(*ea, *ec + 1) == k)
				(*ec)++;
	}
}

static void put_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
	if (*len + 5 > *cap) {
		*cap = MAX(*cap * 2, 256);
		*buf = realloc(*buf, *cap);
	}
	unsigned char *p = (unsigned char *)*buf + *len;
	int n;
	if (cp < 0x80) { p[0] = cp; n = 1; }
	else if (cp < 0x800) { p[0] = 0xc0 | cp >> 6; p[1] = 0x80 | (cp & 0x3f); n = 2; }
	else if (cp < 0x10000) { p[0] = 0xe0 | cp >> 12; p[1] = 0x80 | (cp >> 6 & 0x3f);
		p[2] = 0x80 | (cp & 0x3f); n = 3; }
	else { p[0] = 0xf0 | cp >> 18; p[1] = 0x80 | (cp >> 12 & 0x3f);
		p[2] = 0x80 | (cp >> 6 & 0x3f); p[3] = 0x80 | (cp & 0x3f); n = 4; }
	*len += n;
}

static char *selection_text(void)
{
	if (!t.sel.active)
		return NULL;
	int64_t ba, ea;
	int bc, ec;
	sel_range(&ba, &bc, &ea, &ec);
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (int64_t a = ba; a <= ea; a++) {
		int n;
		const struct cell *l = vt_abs_line(t.vt, a, &n);
		if (!l)
			continue;
		int c0 = a == ba ? bc : 0;
		int c1 = a == ea ? MIN(ec, n - 1) : n - 1;
		size_t start = len;
		for (int c = c0; c <= c1; c++)
			if (!(l[c].attr & ATTR_WDUMMY))
				put_utf8(&buf, &len, &cap, l[c].cp ? l[c].cp : ' ');
		bool wrapped = n > 0 && c1 == n - 1 && (l[n - 1].attr & ATTR_WRAP);
		if (!wrapped) {
			while (len > start && buf[len - 1] == ' ')
				len--;
			if (a != ea)
				put_utf8(&buf, &len, &cap, '\n');
		}
	}
	put_utf8(&buf, &len, &cap, 0);
	return buf;
}

static void sel_clear(void)
{
	if (t.sel.active || t.sel.dragging) {
		t.sel.active = t.sel.dragging = false;
		vt_mark_all(t.vt);
		t.need_redraw = true;
	}
}

/* ---- clipboard and primary selection ---------------------------------- */

/* UTF-8 as Latin-1, for STRING: what it cannot hold becomes "?" */
static char *to_latin1(const char *s)
{
	char *out = malloc(strlen(s) + 1);
	if (!out)
		return NULL;
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)s; *p;) {
		uint32_t cp = *p;
		int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
		if (more)
			cp &= 0x3f >> more;
		p++;
		for (; more && (*p & 0xc0) == 0x80; more--)
			cp = cp << 6 | (*p++ & 0x3f);
		out[o++] = cp < 0x80 || (cp >= 0xa0 && cp < 0x100) ? (char)cp : '?';
	}
	out[o] = 0;
	return out;
}

static void send_text(const char *text, const char *mime, int fd)
{
	char *latin1 = text && !strcmp(mime, "STRING") ? to_latin1(text) : NULL;
	if (latin1)
		text = latin1;
	size_t len = text ? strlen(text) : 0, off = 0;
	while (off < len) {
		ssize_t n = write(fd, text + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += n;
	}
	free(latin1);
	close(fd);
}

static int mime_rank(const char *mime)
{
	for (size_t i = 0; i < LEN(mimes); i++)
		if (!strcmp(mime, mimes[i]))
			return (int)i;
	return 99;
}

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
	if (t.clip_src)
		wl_data_source_destroy(t.clip_src);
	t.clip_src = wl_data_device_manager_create_data_source(t.ddm);
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
	if (t.prim_src)
		zwp_primary_selection_source_v1_destroy(t.prim_src);
	t.prim_src = zwp_primary_selection_device_manager_v1_create_source(t.psm);
	for (size_t i = 0; i < LEN(mimes); i++)
		zwp_primary_selection_source_v1_offer(t.prim_src, mimes[i]);
	zwp_primary_selection_source_v1_add_listener(t.prim_src, &ps_listener, NULL);
	zwp_primary_selection_device_v1_set_selection(t.pdev, t.prim_src, t.serial);
}

static void paste_text(const char *s, size_t n)
{
	bool br = t.vt->mode & MODE_BRACKETED;
	char *buf = malloc(n + 16);
	if (!buf)
		return;
	size_t o = 0;
	if (br) {
		memcpy(buf, "\033[200~", 6);
		o = 6;
	}
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\033' || s[i] == 0)
			continue; /* never let pasted text end the bracket */
		if (s[i] == '\n') {
			if (i == 0 || s[i - 1] != '\r')
				buf[o++] = '\r';
		} else {
			buf[o++] = s[i];
		}
	}
	if (br) {
		memcpy(buf + o, "\033[201~", 6);
		o += 6;
	}
	send_input(buf, o);
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
	int *rank = malloc(sizeof *rank);
	*rank = 99;
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
	int *rank = malloc(sizeof *rank);
	*rank = 99;
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
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
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

/* Length of the valid UTF-8 sequence at s, or 0 */
static int utf8_seq(const unsigned char *s, size_t n)
{
	int k = s[0] < 0x80 ? 1 : s[0] >= 0xc2 && s[0] < 0xe0 ? 2 :
		s[0] >= 0xe0 && s[0] < 0xf0 ? 3 : s[0] >= 0xf0 && s[0] < 0xf5 ? 4 : 0;
	if (!k || (size_t)k > n)
		return 0;
	for (int i = 1; i < k; i++)
		if ((s[i] & 0xc0) != 0x80)
			return 0;
	if ((s[0] == 0xe0 && s[1] < 0xa0) || (s[0] == 0xed && s[1] >= 0xa0) ||
	    (s[0] == 0xf0 && s[1] < 0x90) || (s[0] == 0xf4 && s[1] >= 0x90))
		return 0;
	return k;
}

static void read_paste(void)
{
	for (;;) {
		if (t.paste_len + 4096 > t.paste_cap) {
			t.paste_cap = MAX(t.paste_cap * 2, 8192);
			t.paste_buf = realloc(t.paste_buf, t.paste_cap);
		}
		ssize_t n = read(t.paste_fd, t.paste_buf + t.paste_len, 4096);
		if (n > 0) {
			t.paste_len += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		break;
	}
	close(t.paste_fd);
	t.paste_fd = -1;
	/* STRING is Latin-1, and so is what is not UTF-8 whatever its label says:
	 * sources offer one text under every name. C1 controls go. */
	char *buf = NULL;
	size_t len = 0, cap = 0;
	for (size_t i = 0; i < t.paste_len;) {
		const unsigned char *p = (const unsigned char *)t.paste_buf + i;
		int k = t.paste_latin1 ? 0 : utf8_seq(p, t.paste_len - i);
		if (k) {
			uint32_t cp = *p;
			if (k > 1) {
				cp &= 0x3f >> (k - 1);
				for (int j = 1; j < k; j++)
					cp = cp << 6 | (p[j] & 0x3f);
			}
			put_utf8(&buf, &len, &cap, cp);
			i += k;
			continue;
		}
		if (*p < 0x80 || *p >= 0xa0)
			put_utf8(&buf, &len, &cap, *p);
		i++;
	}
	paste_text(buf ? buf : "", len);
	free(buf);
}

/* ---- rendering -------------------------------------------------------- */

static void init_palette(void)
{
	for (int i = 0; i < 16; i++)
		t.palette[i] = colors16[i];
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
		return color_fg;
	if (c == COL_DEFAULT_BG)
		return color_bg;
	return t.palette[c & 0xff];
}

static inline uint32_t blend(uint32_t bg, uint32_t fg, unsigned a)
{
	uint32_t r = ((fg >> 16 & 0xff) * a + (bg >> 16 & 0xff) * (255 - a)) / 255;
	uint32_t g = ((fg >> 8 & 0xff) * a + (bg >> 8 & 0xff) * (255 - a)) / 255;
	uint32_t b = ((fg & 0xff) * a + (bg & 0xff) * (255 - a)) / 255;
	return 0xff000000 | r << 16 | g << 8 | b;
}

static void fill_rect(struct buffer *b, int x, int y, int w, int h, uint32_t c)
{
	int x1 = MIN(x + w, b->w), y1 = MIN(y + h, b->h);
	x = MAX(x, 0);
	y = MAX(y, 0);
	c |= 0xff000000;
	for (int yy = y; yy < y1; yy++) {
		uint32_t *p = b->data + (size_t)yy * b->w;
		for (int xx = x; xx < x1; xx++)
			p[xx] = c;
	}
}

static void blit(struct buffer *b, const struct glyph *g, int x, int y,
		 int cx0, int cx1, int cy0, int cy1, uint32_t fg)
{
	cx1 = MIN(cx1, b->w);
	cy1 = MIN(cy1, b->h);
	for (int gy = 0; gy < g->h; gy++) {
		int py = y - g->top + gy;
		if (py < cy0 || py >= cy1)
			continue;
		uint32_t *row = b->data + (size_t)py * b->w;
		const uint8_t *a = g->alpha + gy * g->w;
		for (int gx = 0; gx < g->w; gx++) {
			int px = x + g->left + gx;
			if (px < cx0 || px >= cx1 || !a[gx])
				continue;
			row[px] = a[gx] == 255 ? (fg | 0xff000000) : blend(row[px], fg, a[gx]);
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

	for (int col = 0; col < vt->cols; col++) {
		struct cell c = l && col < len ? l[col] : blankc;
		if ((c.attr & ATTR_WDUMMY) && col > 0 && l && col - 1 < len &&
		    (l[col - 1].attr & ATTR_WIDE))
			continue; /* drawn together with its left half */
		if (c.attr & ATTR_WDUMMY)
			c = blankc;
		int span = (c.attr & ATTR_WIDE) && col + 1 < vt->cols ? 2 : 1;
		int x0 = t.pad + col * t.cw;
		uint32_t fg = rgb(c.fg), bg = rgb(c.bg);
		if (c.attr & ATTR_REVERSE) {
			uint32_t tmp = fg; fg = bg; bg = tmp;
		}
		if (c.attr & ATTR_DIM)
			fg = blend(bg, fg, 140) & 0xffffff;
		if (sel && abs >= ba && abs <= ea &&
		    !(abs == ba && col + span - 1 < bc) && !(abs == ea && col > ec)) {
			fg = color_sel_fg;
			bg = color_sel_bg;
		}
		bool cur = cur_row && (vt->c.x == col || (span == 2 && vt->c.x == col + 1));
		bool block = cur && t.focused && vt->cursor_style == CURSOR_BLOCK;
		if (block) {
			fg = bg;
			bg = color_cursor;
		}
		fill_rect(b, x0, y0, span * t.cw, t.ch, bg);
		if (c.cp > ' ' && !(c.attr & ATTR_INVISIBLE)) {
			int style = (c.attr & ATTR_BOLD ? 1 : 0) | (c.attr & ATTR_ITALIC ? 2 : 0);
			const struct glyph *g = font_glyph(t.font, c.cp, style);
			if (g->w)
				blit(b, g, x0, y0 + t.ascent, x0, x0 + span * t.cw,
				     y0, y0 + t.ch, fg);
		}
		int thick = MAX(1, t.scale);
		if (c.attr & ATTR_UNDERLINE)
			fill_rect(b, x0, y0 + t.ascent + thick, span * t.cw, thick, fg);
		if (c.attr & ATTR_STRIKE)
			fill_rect(b, x0, y0 + t.ascent * 2 / 3, span * t.cw, thick, fg);
		if (cur && !block) {
			int w = span * t.cw;
			if (!t.focused) {
				fill_rect(b, x0, y0, w, thick, color_cursor);
				fill_rect(b, x0, y0 + t.ch - thick, w, thick, color_cursor);
				fill_rect(b, x0, y0, thick, t.ch, color_cursor);
				fill_rect(b, x0 + w - thick, y0, thick, t.ch, color_cursor);
			} else if (vt->cursor_style == CURSOR_UNDERLINE) {
				fill_rect(b, x0, y0 + t.ch - 2 * thick, w, 2 * thick, color_cursor);
			} else {
				fill_rect(b, x0, y0, 2 * thick, t.ch, color_cursor);
			}
		}
		col += span - 1;
	}
}

static void buffer_release(void *d, struct wl_buffer *wb)
{
	(void)wb;
	((struct buffer *)d)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static bool buffer_init(struct buffer *b, int w, int h)
{
	if (b->wb) {
		wl_buffer_destroy(b->wb);
		munmap(b->data, b->size);
		b->wb = NULL;
	}
	b->size = (size_t)w * h * 4;
	int fd = memfd_create("mwt", MFD_CLOEXEC);
	if (fd < 0)
		return false;
	if (ftruncate(fd, b->size) < 0) {
		close(fd);
		return false;
	}
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED) {
		close(fd);
		return false;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(t.shm, fd, b->size);
	b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wb, &buffer_listener, b);
	b->w = w;
	b->h = h;
	return true;
}

static void frame_done(void *d, struct wl_callback *cb, uint32_t time)
{
	(void)d; (void)time;
	wl_callback_destroy(cb);
	t.frame_cb = NULL;
}
static const struct wl_callback_listener frame_listener = { frame_done };

static void draw(void)
{
	struct vt *vt = t.vt;
	int W = t.width * t.scale, H = t.height * t.scale;
	struct buffer *b = NULL;
	if (t.last && !t.last->busy)
		b = t.last;
	for (int i = 0; !b && i < 2; i++)
		if (!t.bufs[i].busy)
			b = &t.bufs[i];
	if (!b)
		return; /* both held by the compositor; retry on release */

	bool full = false;
	if (b->w != W || b->h != H) {
		if (!buffer_init(b, W, H))
			return;
		full = true;
	}
	if (b != t.last) {
		if (t.last && t.last->w == W && t.last->h == H && !full)
			memcpy(b->data, t.last->data, b->size);
		else
			full = true;
	}
	if (full) {
		fill_rect(b, 0, 0, W, H, color_bg);
		vt_mark_all(vt);
	}

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

	int64_t ba = 0, ea = 0;
	int bc = 0, ec = 0;
	if (t.sel.active)
		sel_range(&ba, &bc, &ea, &ec);
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
		if (!full)
			wl_surface_damage_buffer(t.surface, 0, t.pad + r * t.ch, W,
						 (r1 - r + 1) * t.ch);
		r = r1;
	}
	if (full)
		wl_surface_damage_buffer(t.surface, 0, 0, W, H);

	wl_surface_set_buffer_scale(t.surface, t.scale);
	wl_surface_attach(t.surface, b->wb, 0, 0);
	t.frame_cb = wl_surface_frame(t.surface);
	wl_callback_add_listener(t.frame_cb, &frame_listener, NULL);
	wl_surface_commit(t.surface);
	b->busy = true;
	t.last = b;
	t.need_redraw = false;
}

/* ---- sizing ----------------------------------------------------------- */

static void snap_size(int cols, int rows, int *w, int *h)
{
	*w = (cols * t.cw + 2 * t.pad + t.scale - 1) / t.scale;
	*h = (rows * t.ch + 2 * t.pad + t.scale - 1) / t.scale;
}

static void update_grid(void)
{
	int cols = MAX(1, (t.width * t.scale - 2 * t.pad) / t.cw);
	int rows = MAX(1, (t.height * t.scale - 2 * t.pad) / t.ch);
	if (cols != t.vt->cols || rows != t.vt->rows) {
		vt_resize(t.vt, cols, rows);
		sel_clear();
		struct winsize ws = { rows, cols, t.width * t.scale, t.height * t.scale };
		if (t.pty >= 0)
			ioctl(t.pty, TIOCSWINSZ, &ws);
	}
	vt_mark_all(t.vt);
	t.need_redraw = true;
}

static void load_cursor_theme(void)
{
	if (t.cursor_theme)
		wl_cursor_theme_destroy(t.cursor_theme);
	const char *sz = getenv("XCURSOR_SIZE");
	int size = sz && atoi(sz) > 0 ? atoi(sz) : 24;
	t.cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size * t.scale, t.shm);
	t.cursor_image = NULL;
	if (!t.cursor_theme)
		return;
	static const char *names[] = { "text", "xterm", "left_ptr", "default" };
	for (size_t i = 0; i < LEN(names) && !t.cursor_image; i++) {
		struct wl_cursor *c = wl_cursor_theme_get_cursor(t.cursor_theme, names[i]);
		if (c && c->image_count)
			t.cursor_image = c->images[0];
	}
	if (!t.cursor_image)
		return;
	if (!t.cursor_surface)
		t.cursor_surface = wl_compositor_create_surface(t.compositor);
	wl_surface_set_buffer_scale(t.cursor_surface, t.scale);
	wl_surface_attach(t.cursor_surface, wl_cursor_image_get_buffer(t.cursor_image), 0, 0);
	wl_surface_damage_buffer(t.cursor_surface, 0, 0, INT32_MAX, INT32_MAX);
	wl_surface_commit(t.cursor_surface);
}

static void reload_font(double pt, int scale)
{
	struct font *f = font_open(t.font_spec, pt, scale);
	if (!f)
		return;
	font_close(t.font);
	t.font = f;
	t.font_pt = font_size_pt(f);
	bool rescale = scale != t.scale;
	t.scale = scale;
	font_metrics(f, &t.cw, &t.ch, &t.ascent);
	t.pad = padding * scale;
	if (rescale && t.shm)
		load_cursor_theme();
	if (t.configured) {
		if (!t.cfg_fixed) /* keep columns and rows, the window follows */
			snap_size(t.vt->cols, t.vt->rows, &t.width, &t.height);
		update_grid();
	}
}

static void zoom(int delta)
{
	double pt = delta ? t.font_pt + delta : t.font_pt0;
	if (pt >= 4 && pt <= 96)
		reload_font(pt, t.scale);
}

/* ---- keyboard --------------------------------------------------------- */

static bool mod_active(const char *name)
{
	return xkb_state_mod_name_is_active(t.xkb_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

static void cursor_key(char ch, int m)
{
	char buf[16];
	if (m > 1)
		snprintf(buf, sizeof buf, "\033[1;%d%c", m, ch);
	else
		snprintf(buf, sizeof buf, (t.vt->mode & MODE_APPCURSOR) ? "\033O%c" : "\033[%c", ch);
	send_input(buf, strlen(buf));
}

static void tilde_key(int n, int m)
{
	char buf[16];
	if (m > 1)
		snprintf(buf, sizeof buf, "\033[%d;%d~", n, m);
	else
		snprintf(buf, sizeof buf, "\033[%d~", n);
	send_input(buf, strlen(buf));
}

static void fn_key(char ch, int m)
{
	char buf[16];
	if (m > 1)
		snprintf(buf, sizeof buf, "\033[1;%d%c", m, ch);
	else
		snprintf(buf, sizeof buf, "\033O%c", ch);
	send_input(buf, strlen(buf));
}

static void send_alt(bool alt, const char *s, size_t n)
{
	char buf[80];
	if (n + 1 > sizeof buf)
		return;
	if (!alt && n == 1)
		drop_paste_for_signal((unsigned char)s[0]);
	size_t o = 0;
	if (alt)
		buf[o++] = '\033';
	memcpy(buf + o, s, n);
	send_input(buf, o + n);
}

static xkb_keysym_t base_sym(xkb_keycode_t code)
{
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout = xkb_state_key_get_layout(t.xkb_state, code);
	if (xkb_keymap_key_get_syms_by_level(t.keymap, code, layout, 0, &syms) > 0)
		return syms[0];
	return XKB_KEY_NoSymbol;
}

/*
 * With Ctrl, a letter that is not Latin (a Russian or Greek layout) still means
 * its Latin letter, or Ctrl+C would not interrupt anything: the one in another
 * layout of the keymap, else the one at that place on a QWERTY keyboard. Keys
 * without a Latin place keep what they are, like the German umlauts.
 */
static xkb_keysym_t ctrl_latin(xkb_keycode_t code, xkb_keysym_t sym)
{
	if (xkb_keysym_to_utf32(sym) < 0x80) /* ASCII, or no text at all */
		return sym;
	xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(t.keymap, code);
	for (xkb_layout_index_t l = 0; l < layouts; l++) {
		const xkb_keysym_t *syms;
		if (xkb_keymap_key_get_syms_by_level(t.keymap, code, l, 0, &syms) == 1 &&
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

static void handle_key(xkb_keycode_t code)
{
	if (!t.xkb_state)
		return;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(t.xkb_state, code);
	bool ctrl = mod_active(XKB_MOD_NAME_CTRL);
	bool alt = mod_active(XKB_MOD_NAME_ALT);
	bool shift = mod_active(XKB_MOD_NAME_SHIFT);
	int m = 1 + shift + 2 * alt + 4 * ctrl;

	if (ctrl && shift) {
		switch (ctrl_latin(code, base_sym(code))) {
		case XKB_KEY_c: copy_clipboard(); return;
		case XKB_KEY_v: paste(false); return;
		case XKB_KEY_plus: case XKB_KEY_equal: zoom(1); return;
		case XKB_KEY_minus: zoom(-1); return;
		case XKB_KEY_0: zoom(0); return;
		}
	}
	if (shift && !ctrl && !alt) {
		switch (sym) {
		case XKB_KEY_Insert: case XKB_KEY_KP_Insert:
			paste(true);
			return;
		case XKB_KEY_Prior: case XKB_KEY_KP_Prior:
			vt_scroll_view(t.vt, t.vt->rows - 1);
			t.need_redraw = true;
			return;
		case XKB_KEY_Next: case XKB_KEY_KP_Next:
			vt_scroll_view(t.vt, -(t.vt->rows - 1));
			t.need_redraw = true;
			return;
		}
	}

	switch (sym) {
	case XKB_KEY_Up: case XKB_KEY_KP_Up: cursor_key('A', m); return;
	case XKB_KEY_Down: case XKB_KEY_KP_Down: cursor_key('B', m); return;
	case XKB_KEY_Right: case XKB_KEY_KP_Right: cursor_key('C', m); return;
	case XKB_KEY_Left: case XKB_KEY_KP_Left: cursor_key('D', m); return;
	case XKB_KEY_Home: case XKB_KEY_KP_Home: cursor_key('H', m); return;
	case XKB_KEY_End: case XKB_KEY_KP_End: cursor_key('F', m); return;
	case XKB_KEY_Insert: case XKB_KEY_KP_Insert: tilde_key(2, m); return;
	case XKB_KEY_Delete: case XKB_KEY_KP_Delete: tilde_key(3, m); return;
	case XKB_KEY_Prior: case XKB_KEY_KP_Prior: tilde_key(5, m); return;
	case XKB_KEY_Next: case XKB_KEY_KP_Next: tilde_key(6, m); return;
	case XKB_KEY_F1: fn_key('P', m); return;
	case XKB_KEY_F2: fn_key('Q', m); return;
	case XKB_KEY_F3: fn_key('R', m); return;
	case XKB_KEY_F4: fn_key('S', m); return;
	case XKB_KEY_F5: tilde_key(15, m); return;
	case XKB_KEY_F6: tilde_key(17, m); return;
	case XKB_KEY_F7: tilde_key(18, m); return;
	case XKB_KEY_F8: tilde_key(19, m); return;
	case XKB_KEY_F9: tilde_key(20, m); return;
	case XKB_KEY_F10: tilde_key(21, m); return;
	case XKB_KEY_F11: tilde_key(23, m); return;
	case XKB_KEY_F12: tilde_key(24, m); return;
	case XKB_KEY_BackSpace: send_alt(alt, ctrl ? "\b" : "\x7f", 1); return;
	case XKB_KEY_Return: case XKB_KEY_KP_Enter: send_alt(alt, "\r", 1); return;
	case XKB_KEY_ISO_Left_Tab: send_input("\033[Z", 3); return;
	case XKB_KEY_Tab: send_alt(alt, shift ? "\033[Z" : "\t", shift ? 3 : 1); return;
	case XKB_KEY_Escape: send_alt(alt, "\033", 1); return;
	}

	if (ctrl) {
		xkb_keysym_t latin = ctrl_latin(code, sym);
		if (latin != sym && latin >= XKB_KEY_a && latin <= XKB_KEY_z) {
			char c = (char)(latin & 0x1f);
			send_alt(alt, &c, 1);
			return;
		}
	}

	char buf[64];
	int n = 0;
	if (t.compose) {
		xkb_compose_state_feed(t.compose, sym);
		switch (xkb_compose_state_get_status(t.compose)) {
		case XKB_COMPOSE_COMPOSING:
			return;
		case XKB_COMPOSE_CANCELLED:
			xkb_compose_state_reset(t.compose);
			return;
		case XKB_COMPOSE_COMPOSED:
			n = xkb_compose_state_get_utf8(t.compose, buf, sizeof buf);
			xkb_compose_state_reset(t.compose);
			break;
		case XKB_COMPOSE_NOTHING:
			break;
		}
	}
	if (!n)
		n = xkb_state_key_get_utf8(t.xkb_state, code, buf, sizeof buf);
	if (n <= 0) {
		if (ctrl && (sym == XKB_KEY_space || sym == XKB_KEY_at || sym == XKB_KEY_2))
			send_alt(alt, "", 1); /* NUL */
		return;
	}
	send_alt(alt, buf, MIN((size_t)n, sizeof buf - 1));
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
	struct xkb_keymap *km = xkb_keymap_new_from_string(t.xkb, map,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(map, size);
	if (!km)
		return;
	xkb_state_unref(t.xkb_state);
	xkb_keymap_unref(t.keymap);
	t.keymap = km;
	t.xkb_state = xkb_state_new(km);
}

static void kb_enter(void *d, struct wl_keyboard *kb, uint32_t serial,
		     struct wl_surface *s, struct wl_array *keys)
{
	(void)d; (void)kb; (void)s; (void)keys;
	t.serial = serial;
	t.focused = true;
	t.vt->dirty[t.vt->c.y] = true;
	t.need_redraw = true;
	if (t.vt->mode & MODE_FOCUS)
		pty_puts("\033[I");
}

static void stop_repeat(void)
{
	struct itimerspec its = { 0 };
	timerfd_settime(t.repeat_fd, 0, &its, NULL);
	t.repeat_key = 0;
}

static void kb_leave(void *d, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)kb; (void)serial; (void)s;
	t.focused = false;
	stop_repeat();
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
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		handle_key(code);
		if (t.repeat_rate > 0 && t.keymap && xkb_keymap_key_repeats(t.keymap, code)) {
			t.repeat_key = code;
			long iv = 1000000000L / t.repeat_rate;
			struct itimerspec its = {
				.it_interval = { iv / 1000000000L, iv % 1000000000L },
				.it_value = { t.repeat_delay / 1000, (t.repeat_delay % 1000) * 1000000L },
			};
			timerfd_settime(t.repeat_fd, 0, &its, NULL);
		}
	} else if (code == t.repeat_key) {
		stop_repeat();
	}
}

static void kb_modifiers(void *d, struct wl_keyboard *kb, uint32_t serial, uint32_t dep,
			 uint32_t lat, uint32_t lock, uint32_t group)
{
	(void)d; (void)kb; (void)serial;
	if (t.xkb_state)
		xkb_state_update_mask(t.xkb_state, dep, lat, lock, 0, 0, group);
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
	return (t.vt->mode & MODE_MOUSE) && !(t.xkb_state && mod_active(XKB_MOD_NAME_SHIFT));
}

static void mouse_report(int code, bool press)
{
	if (t.xkb_state) {
		code |= mod_active(XKB_MOD_NAME_SHIFT) ? 4 : 0;
		code |= mod_active(XKB_MOD_NAME_ALT) ? 8 : 0;
		code |= mod_active(XKB_MOD_NAME_CTRL) ? 16 : 0;
	}
	char buf[40];
	int x = t.mcol + 1, y = t.mrow + 1;
	if (t.vt->mode & MODE_MOUSE_SGR) {
		snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", code, x, y, press ? 'M' : 'm');
		pty_puts(buf);
		return;
	}
	if (x > 223 || y > 223)
		return;
	if (!press)
		code = (code & ~3) | 3;
	snprintf(buf, sizeof buf, "\033[M%c%c%c", 32 + code, 32 + x, 32 + y);
	pty_puts(buf);
}

static void pointer_cell(int *col, int *row)
{
	int x = (int)(t.px * t.scale) - t.pad, y = (int)(t.py * t.scale) - t.pad;
	*col = x < 0 ? 0 : MIN(x / t.cw, t.vt->cols - 1);
	*row = y < 0 ? 0 : MIN(y / t.ch, t.vt->rows - 1);
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
		      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)s;
	t.enter_serial = serial;
	t.px = wl_fixed_to_double(x);
	t.py = wl_fixed_to_double(y);
	pointer_cell(&t.mcol, &t.mrow);
	if (t.cursor_image)
		wl_pointer_set_cursor(p, serial, t.cursor_surface,
				      t.cursor_image->hotspot_x / t.scale,
				      t.cursor_image->hotspot_y / t.scale);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	(void)d; (void)p; (void)serial; (void)s;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	(void)d; (void)p; (void)time;
	t.px = wl_fixed_to_double(x);
	t.py = wl_fixed_to_double(y);
	int col, row;
	pointer_cell(&col, &row);
	bool moved = col != t.mcol || row != t.mrow;
	t.mcol = col;
	t.mrow = row;

	if (mouse_mode()) {
		unsigned m = t.vt->mode;
		if (moved && ((m & MODE_MOUSE_ANY) || ((m & MODE_MOUSE_DRAG) && t.buttons))) {
			int btn = t.buttons & 1 ? 0 : t.buttons & 2 ? 1 : t.buttons & 4 ? 2 : 3;
			mouse_report(btn + 32, true);
		}
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
	if (abs != t.sel.ea || col != t.sel.ec || !t.sel.active) {
		t.sel.ea = abs;
		t.sel.ec = col;
		t.sel.active = true;
		vt_mark_all(t.vt);
		t.need_redraw = true;
	}
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time,
		       uint32_t button, uint32_t state)
{
	(void)d; (void)p;
	int b = button == BTN_LEFT ? 0 : button == BTN_MIDDLE ? 1 : button == BTN_RIGHT ? 2 : -1;
	if (b < 0)
		return;
	t.serial = serial;
	bool press = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (press)
		t.buttons |= 1u << b;
	else
		t.buttons &= ~(1u << b);

	if (mouse_mode()) {
		mouse_report(b, press);
		return;
	}
	int64_t abs = (int64_t)t.vt->scrolled - t.vt->view + t.mrow;
	if (b == 0 && press) {
		if (time - t.click_time < 400 && abs == t.click_abs && t.mcol == t.click_col)
			t.click_count = t.click_count % 3 + 1;
		else
			t.click_count = 1;
		t.click_time = time;
		t.click_abs = abs;
		t.click_col = t.mcol;
		t.sel.mode = t.click_count;
		t.sel.aa = t.sel.ea = abs;
		t.sel.ac = t.sel.ec = t.mcol;
		t.sel.dragging = true;
		t.sel.active = t.click_count > 1;
		vt_mark_all(t.vt);
		t.need_redraw = true;
	} else if (b == 0) {
		t.sel.dragging = false;
		if (t.sel.active)
			copy_primary();
	} else if (b == 1 && press) {
		paste(true);
	} else if (b == 2 && press && t.sel.active) {
		t.sel.ea = abs; /* right click extends the selection */
		t.sel.ec = t.mcol;
		vt_mark_all(t.vt);
		t.need_redraw = true;
		copy_primary();
	}
}

static void wheel(bool down)
{
	if (mouse_mode()) {
		mouse_report(down ? 65 : 64, true);
	} else if (t.vt->mode & MODE_ALTSCREEN) {
		for (int i = 0; i < wheel_lines; i++)
			cursor_key(down ? 'B' : 'A', 1);
	} else {
		vt_scroll_view(t.vt, down ? -wheel_lines : wheel_lines);
		t.need_redraw = true;
	}
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	(void)d; (void)p; (void)time;
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	t.axis_acc += wl_fixed_to_double(v);
	while (t.axis_acc >= 10) {
		t.axis_acc -= 10;
		wheel(true);
	}
	while (t.axis_acc <= -10) {
		t.axis_acc += 10;
		wheel(false);
	}
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t tm, uint32_t a)
{
	(void)d; (void)p; (void)tm; (void)a;
	t.axis_acc = 0;
}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t n)
{
	(void)d; (void)p; (void)a; (void)n;
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
		wl_keyboard_release(t.keyboard);
		t.keyboard = NULL;
	}
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !t.pointer) {
		t.pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(t.pointer, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && t.pointer) {
		wl_pointer_release(t.pointer);
		t.pointer = NULL;
	}
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
	int w = t.cfg_w, h = t.cfg_h;
	if (w <= 0 || h <= 0) {
		if (t.configured) {
			w = t.width;
			h = t.height;
		} else {
			snap_size(t.init_cols, t.init_rows, &w, &h);
		}
	} else if (!t.cfg_fixed) {
		int cols = MAX(1, (w * t.scale - 2 * t.pad) / t.cw);
		int rows = MAX(1, (h * t.scale - 2 * t.pad) / t.ch);
		snap_size(cols, rows, &w, &h);
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
	(void)d; (void)tl; (void)w; (void)h;
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
		wl_seat_add_listener(t.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		t.deco_mgr = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
		t.ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, MIN(ver, 3));
	} else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) {
		t.psm = wl_registry_bind(r, name,
			&zwp_primary_selection_device_manager_v1_interface, 1);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d; (void)r; (void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ---- pty and child ---------------------------------------------------- */

static bool have_terminfo(const char *name)
{
	char path[4096];
	const char *home = getenv("HOME");
	const char *dirs[16];
	int n = 0;
	if (getenv("TERMINFO"))
		dirs[n++] = getenv("TERMINFO");
	char homedir[4096];
	if (home) {
		snprintf(homedir, sizeof homedir, "%s/.terminfo", home);
		dirs[n++] = homedir;
	}
	char *tdirs = getenv("TERMINFO_DIRS") ? strdup(getenv("TERMINFO_DIRS")) : NULL;
	for (char *tok = tdirs ? strtok(tdirs, ":") : NULL; tok && n < 10; tok = strtok(NULL, ":"))
		dirs[n++] = tok;
	static const char *std[] = { "/etc/terminfo", "/lib/terminfo", "/usr/share/terminfo",
		"/usr/lib/terminfo", "/usr/local/share/terminfo" };
	for (size_t i = 0; i < LEN(std); i++)
		dirs[n++] = std[i];
	bool found = false;
	for (int i = 0; i < n && !found; i++) {
		snprintf(path, sizeof path, "%s/%c/%s", dirs[i], name[0], name);
		found = access(path, R_OK) == 0;
		snprintf(path, sizeof path, "%s/%02x/%s", dirs[i], (unsigned char)name[0], name);
		found |= access(path, R_OK) == 0;
	}
	free(tdirs);
	return found;
}

static void spawn(char **argv)
{
	struct winsize ws = { t.vt->rows, t.vt->cols, 0, 0 };
	const char *term = have_terminfo(term_name) ? term_name : "xterm-256color";
	if (term != term_name)
		fprintf(stderr, "mwt: no terminfo for '%s', using %s "
			"(run 'tic -x mwt.info')\n", term_name, term);
	t.child = forkpty(&t.pty, NULL, NULL, &ws);
	if (t.child < 0)
		die("forkpty failed");
	if (t.child == 0) {
		setenv("TERM", term, 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");
		/* what mwt ignores or was started ignoring is not for the shell:
		 * `mwt &` from sh ignores SIGINT and SIGQUIT */
		for (int sig = 1; sig < NSIG; sig++)
			signal(sig, SIG_DFL);
		if (argv && argv[0]) {
			execvp(argv[0], argv);
			perror(argv[0]);
		} else {
			const char *sh = getenv("SHELL");
			struct passwd *pw = getpwuid(getuid());
			if (!sh || !*sh)
				sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
			execl(sh, sh, (char *)NULL);
			perror(sh);
		}
		_exit(127);
	}
	fcntl(t.pty, F_SETFL, fcntl(t.pty, F_GETFL) | O_NONBLOCK);
	fcntl(t.pty, F_SETFD, FD_CLOEXEC);
	t.child_fd = (int)syscall(SYS_pidfd_open, t.child, 0);
}

static void read_pty(void)
{
	static char buf[65536];
	size_t total = 0;
	while (total < (1u << 20)) {
		ssize_t n = read(t.pty, buf, sizeof buf);
		if (n > 0) {
			vt_write(t.vt, buf, n);
			total += n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		t.running = false; /* EOF or EIO: the child is gone */
		break;
	}
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
	fprintf(stderr, "usage: mwt [-f font] [-a app-id] [-T title] [-g COLSxROWS] "
		"[-e] [command [args...]]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	const char *app_id = default_app_id, *title = default_title;
	t.font_spec = font_spec;
	t.init_cols = default_cols;
	t.init_rows = default_rows;
	int opt;
	while ((opt = getopt(argc, argv, "+f:a:T:g:eh")) != -1) {
		switch (opt) {
		case 'f': t.font_spec = optarg; break;
		case 'a': app_id = optarg; break;
		case 'T': title = optarg; break;
		case 'g':
			if (sscanf(optarg, "%dx%d", &t.init_cols, &t.init_rows) != 2 ||
			    t.init_cols < 1 || t.init_rows < 1)
				usage();
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

	init_palette();
	t.font = font_open(t.font_spec, 0, 1);
	if (!t.font)
		die("cannot load font");
	t.font_pt = t.font_pt0 = font_size_pt(t.font);
	font_metrics(t.font, &t.cw, &t.ch, &t.ascent);
	t.pad = padding;

	struct vt_callbacks cb = { .write = vt_reply, .title = vt_title };
	t.vt = vt_new(t.init_cols, t.init_rows, histsize, &cb);
	if (!t.vt)
		die("out of memory");

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
	t.repeat_rate = 25;
	t.repeat_delay = 600;

	t.display = wl_display_connect(NULL);
	if (!t.display)
		die("cannot connect to Wayland display");
	struct wl_registry *reg = wl_display_get_registry(t.display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(t.display);
	if (!t.compositor || !t.shm || !t.wm_base)
		die("compositor lacks wl_compositor, wl_shm or xdg_wm_base");
	if (t.seat && t.ddm) {
		t.ddev = wl_data_device_manager_get_data_device(t.ddm, t.seat);
		wl_data_device_add_listener(t.ddev, &dd_listener, NULL);
	}
	if (t.seat && t.psm) {
		t.pdev = zwp_primary_selection_device_manager_v1_get_device(t.psm, t.seat);
		zwp_primary_selection_device_v1_add_listener(t.pdev, &pd_listener, NULL);
	}
	load_cursor_theme();

	t.surface = wl_compositor_create_surface(t.compositor);
	wl_surface_add_listener(t.surface, &surf_listener, NULL);
	t.xsurface = xdg_wm_base_get_xdg_surface(t.wm_base, t.surface);
	xdg_surface_add_listener(t.xsurface, &xs_listener, NULL);
	t.toplevel = xdg_surface_get_toplevel(t.xsurface);
	xdg_toplevel_add_listener(t.toplevel, &tl_listener, NULL);
	xdg_toplevel_set_app_id(t.toplevel, app_id);
	xdg_toplevel_set_title(t.toplevel, title);
	if (t.deco_mgr) {
		t.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(t.deco_mgr, t.toplevel);
		zxdg_toplevel_decoration_v1_set_mode(t.deco,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
	wl_surface_commit(t.surface);
	wl_display_roundtrip(t.display);

	spawn(cmd);
	t.running = true;
	int wlfd = wl_display_get_fd(t.display);

	while (t.running) {
		if (t.need_redraw && !t.frame_cb && t.configured)
			draw();
		while (wl_display_prepare_read(t.display) != 0)
			wl_display_dispatch_pending(t.display);
		bool wl_out = wl_display_flush(t.display) < 0 && errno == EAGAIN;

		struct pollfd fds[5] = {
			{ wlfd, POLLIN | (wl_out ? POLLOUT : 0), 0 },
			{ t.pty, POLLIN | (t.out_len ? POLLOUT : 0), 0 },
			{ t.repeat_fd, POLLIN, 0 },
			{ t.paste_fd, POLLIN, 0 },
			{ t.child_fd, POLLIN, 0 },
		};
		if (poll(fds, 5, -1) < 0) {
			wl_display_cancel_read(t.display);
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[0].revents & POLLIN)
			wl_display_read_events(t.display);
		else
			wl_display_cancel_read(t.display);
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(t.display) < 0)
			break;

		if (fds[1].revents & (POLLIN | POLLHUP | POLLERR))
			read_pty();
		if (fds[1].revents & POLLOUT)
			flush_out();
		if (fds[2].revents & POLLIN) {
			uint64_t n = 0;
			if (read(t.repeat_fd, &n, sizeof n) == sizeof n && t.repeat_key)
				for (uint64_t i = 0; i < MIN(n, 32u); i++)
					handle_key(t.repeat_key);
		}
		if (t.paste_fd >= 0 && (fds[3].revents & (POLLIN | POLLHUP | POLLERR)))
			read_paste();
		if (fds[4].revents & POLLIN)
			t.running = false; /* the pty may stay open, held by jobs */
	}

	if (t.child > 0) {
		kill(t.child, SIGHUP);
		waitpid(t.child, NULL, 0);
	}
	wl_display_disconnect(t.display);
	return 0;
}
