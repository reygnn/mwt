/* fuzz_vt.c - coverage-guided fuzzing of the VT core with libFuzzer.
 *
 * The first two bytes pick the grid (1..140 x 1..60), the rest goes into two
 * terminals: one gets it in a single write, the other in chunks whose lengths
 * come from the input itself. Both must end in the same state, replies, title
 * and bells included, and after every
 * write the grid must be consistent (see check()). Then the second terminal is
 * resized a few times, the view scrolled through the history and the terminal
 * reset, each followed by the same checks. Sanitizers catch the rest.
 *
 *   make fuzz                     ten minutes, corpus in tests/fuzz/corpus
 *   make fuzz FUZZ_TIME=3600      one hour
 *   ./fuzz_vt tests/fuzz/crash-…  replay one finding (make fuzz_vt, needs clang)
 *
 * make fuzz runs it in the container of make evil when the host has no clang
 * with libFuzzer, see tests/fuzz.sh. It starts from tests/fuzz-regress and from
 * tests/vt100 if `make replay` fetched it.
 *
 * Built with -DFUZZ_STANDALONE it needs no clang: a main() of its own feeds the
 * files it is given through the same checks. `make check` runs it over
 * tests/fuzz-regress, the findings of make fuzz kept for good. To keep one:
 *
 *   ./fuzz_vt -minimize_crash=1 -runs=100000 tests/fuzz/crash-…   (while it
 *       still fails; the shortest input lands in tests/fuzz/minimized-from-…)
 *   fix vt.c, then copy the input to tests/fuzz-regress/ under a name that
 *   says what it was, e.g. decstbm-bottom-not-clamped
 *
 * The first five there come from bugs planted on purpose (parser state lost
 * between writes, DECSTBM bottom not clamped, OSC 4 index not checked, ICH not
 * raising the row's count of written cells, the main screen's cursor row not
 * moved by a resize behind the alternate screen), each caught by its file. They
 * depend on how this harness reads an input: change the header or the chunking,
 * and they may no longer mean what they meant. */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST 64

/* what a terminal said: a hash of its replies, its last title, its bells */
struct said {
	uint64_t replies;
	size_t reply_bytes;
	char title[VT_OSC_MAX + 1];
	int titles, bells;
};

static void on_write(void *ud, const char *b, size_t n)
{
	struct said *s = ud;
	for (size_t i = 0; i < n; i++)
		s->replies = (s->replies ^ (unsigned char)b[i]) * 0x100000001b3u;
	s->reply_bytes += n;
}

static void on_title(void *ud, const char *t)
{
	struct said *s = ud;
	snprintf(s->title, sizeof s->title, "%s", t);
	s->titles++;
}

static void on_bell(void *ud)
{
	((struct said *)ud)->bells++;
}

static uint32_t on_color(void *ud, uint32_t c)
{
	(void)ud;
	if (c != COL_DEFAULT_FG && c != COL_DEFAULT_BG && c != COL_CURSOR && c > 255)
		abort(); /* vt.c asked for a color that does not exist */
	return c * 0x10101u;
}

static struct vt *mk(int cols, int rows, struct said *s)
{
	*s = (struct said){ .replies = 0xcbf29ce484222325u };
	struct vt_callbacks cb = { .write = on_write, .title = on_title, .bell = on_bell,
				   .color = on_color, .ud = s };
	return vt_new(cols, rows, HIST, &cb);
}

static void die(const char *what, const struct vt *vt)
{
	fprintf(stderr, "fuzz_vt: %s (grid %dx%d, cursor %d,%d, region %d-%d)\n",
		what, vt->cols, vt->rows, vt->c.x, vt->c.y, vt->top, vt->bot);
	abort();
}

/* What is none of a row's content at its end (RESIZE.md 1.1): a blank of its
 * tail, not written. */
static bool end_blank(struct cell c, uint32_t tail)
{
	return c.cp == ' ' && c.fg == COL_DEFAULT_FG && c.bg == tail && !c.attr;
}

static void check(const struct vt *vt)
{
	if (vt->c.x < 0 || vt->c.x >= vt->cols || vt->c.y < 0 || vt->c.y >= vt->rows)
		die("cursor outside the screen", vt);
	if (vt->top < 0 || vt->bot >= vt->rows || vt->top > vt->bot)
		die("scroll region outside the screen", vt);
	if (vt->c.gl < 0 || vt->c.gl > 1)
		die("GL is neither G0 nor G1", vt);
	if (vt->view < 0 || vt->view > vt->hist_count)
		die("view outside the history", vt);
	/* the row a resize keeps on the main screen while the alternate one shows */
	if ((vt->mode & MODE_ALTSCREEN) && (vt->pri_y < 0 || vt->pri_y >= vt->rows ||
					    vt->pri_x < 0 || vt->pri_x > vt->cols))
		die("the main screen's cursor behind the alternate one outside the screen", vt);
	/* a history line wraps within what it has */
	for (int i = 0; i < vt->hist_count; i++) {
		int slot = (vt->hist_head - 1 - i + vt->hist_cap) % vt->hist_cap;
		if (vt->hist_wrap[slot] < 0 || vt->hist_wrap[slot] > vt->hist_len[slot])
			die("history line wraps past its end", vt);
		/* laid out for the screen's width, as the main screen is */
		if (vt->hist_len[slot] > vt->cols)
			die("history line longer than the screen is wide", vt);
		/* a row whose end was erased in a colour has cells to its last column */
		if (vt->hist_tail[slot] != COL_DEFAULT_BG && vt->hist_len[slot] != vt->cols)
			die("history line with a tail in a colour is not full", vt);
		for (int x = vt->hist_wrap[slot]; vt->hist_wrap[slot] && x < vt->hist_len[slot]; x++)
			if (!end_blank(vt->hist[slot][x], vt->hist_tail[slot]))
				die("cell behind a history line's wrap is not of its tail", vt);
	}
	struct row **screens[2] = { vt->pri, vt->alt };
	for (int s = 0; s < 2; s++)
		for (int r = 0; r < vt->rows; r++) {
			const struct cell *l = screens[s][r]->c;
			/* from the row's count on, only default blanks */
			int used = screens[s][r]->used, wrap = screens[s][r]->wrap;
			if (used < 0 || used > vt->cols)
				die("row written past its end", vt);
			if (wrap < 0 || wrap > used)
				die("row wraps past what was written", vt);
			if (screens[s][r]->tail != COL_DEFAULT_BG && used != vt->cols)
				die("row with a tail in a colour is not full", vt);
			/* nothing is checked where a wrap is read (RESIZE.md I2) */
			for (int x = wrap; wrap && x < vt->cols; x++)
				if (!end_blank(l[x], screens[s][r]->tail))
					die("cell behind a row's wrap is not of its tail", vt);
			/* the last row of a screen does not go on (I3) */
			if (r == vt->rows - 1 && wrap)
				die("last row of a screen goes on", vt);
			/* On the main screen a join short of the last column is one
			 * the layout would make: into a row without content or, one
			 * cell short, into one that begins with a wide glyph; from
			 * the history's newest line into the first row too (I8). */
			int join = s ? 0 : r ? screens[s][r - 1]->wrap : vt->hist_count ?
				vt->hist_wrap[(vt->hist_head - 1 + vt->hist_cap) % vt->hist_cap] : 0;
			if (join && join != vt->cols) {
				int n = used;
				while (n > 0 && end_blank(l[n - 1], screens[s][r]->tail))
					n--;
				if ((wrap || n) && !(join == vt->cols - 1 && (l[0].attr & ATTR_WIDE)))
					die("join the layout would not make at this width", vt);
			}
			for (int x = used; x < vt->cols; x++)
				if (l[x].cp != ' ' || l[x].fg != COL_DEFAULT_FG ||
				    l[x].bg != COL_DEFAULT_BG || l[x].attr)
					die("cell after the written part of a row is not blank", vt);
			for (int x = 0; x < vt->cols; x++) {
				bool wide = l[x].attr & ATTR_WIDE;
				bool dummy = l[x].attr & ATTR_WDUMMY;
				if (wide && (x + 1 >= vt->cols || !(l[x + 1].attr & ATTR_WDUMMY)))
					die("wide cell without right half", vt);
				if (dummy && (x == 0 || !(l[x - 1].attr & ATTR_WIDE)))
					die("right half without wide cell", vt);
				if (l[x].cp >= 0x80 && l[x].cp < 0xa0)
					die("C1 control stored as text", vt);
				/* an erase leaves blanks, in its background alone */
				if (!(l[x].attr & ATTR_TEXT) &&
				    (l[x].cp != ' ' || l[x].fg != COL_DEFAULT_FG || l[x].attr))
					die("cell that was not written is no blank", vt);
			}
		}
}

static bool same_cells(const struct cell *a, const struct cell *b, int n)
{
	for (int i = 0; i < n; i++)
		if (a[i].cp != b[i].cp || a[i].fg != b[i].fg || a[i].bg != b[i].bg ||
		    a[i].attr != b[i].attr)
			return false;
	return true;
}

static bool same_cursor(const struct vt_cursor *a, const struct vt_cursor *b)
{
	return a->x == b->x && a->y == b->y && a->wrapnext == b->wrapnext &&
	       same_cells(&a->tmpl, &b->tmpl, 1) && a->charset[0] == b->charset[0] &&
	       a->charset[1] == b->charset[1] && a->gl == b->gl && a->origin == b->origin;
}

static void compare(const struct vt *a, const struct vt *b,
		    const struct said *sa, const struct said *sb)
{
	if (a->mode != b->mode)
		die("modes differ between whole and chunked", b);
	if (!same_cursor(&a->c, &b->c))
		die("cursor differs between whole and chunked", b);
	if (!same_cursor(&a->saved_pri, &b->saved_pri) || !same_cursor(&a->saved_alt, &b->saved_alt))
		die("saved cursor differs between whole and chunked", b);
	if ((a->mode & MODE_ALTSCREEN) && (a->pri_x != b->pri_x || a->pri_y != b->pri_y))
		die("the main screen's cursor behind the alternate one differs between whole and chunked", b);
	if (a->top != b->top || a->bot != b->bot)
		die("scroll region differs between whole and chunked", b);
	if (a->cursor_style != b->cursor_style)
		die("cursor style differs between whole and chunked", b);
	if (memcmp(a->tabs, b->tabs, (size_t)a->cols * sizeof *a->tabs))
		die("tab stops differ between whole and chunked", b);
	for (int r = 0; r < a->rows; r++)
		if (!same_cells(a->pri[r]->c, b->pri[r]->c, a->cols) ||
		    !same_cells(a->alt[r]->c, b->alt[r]->c, a->cols) ||
		    a->pri[r]->wrap != b->pri[r]->wrap || a->alt[r]->wrap != b->alt[r]->wrap ||
		    a->pri[r]->tail != b->pri[r]->tail || a->alt[r]->tail != b->alt[r]->tail)
			die("screen differs between whole and chunked", b);
	if (a->hist_count != b->hist_count || a->scrolled != b->scrolled)
		die("history differs between whole and chunked", b);
	for (int64_t i = vt_abs_first(a); i < vt_abs_first(a) + a->hist_count; i++) {
		int la, lb;
		const struct cell *ca = vt_abs_line(a, i, &la), *cb = vt_abs_line(b, i, &lb);
		if (!ca || !cb || la != lb || !same_cells(ca, cb, la) ||
		    vt_abs_wraps(a, i) != vt_abs_wraps(b, i))
			die("history lines differ between whole and chunked", b);
	}
	if (sa->replies != sb->replies || sa->reply_bytes != sb->reply_bytes)
		die("replies differ between whole and chunked", b);
	if (sa->titles != sb->titles || strcmp(sa->title, sb->title))
		die("title differs between whole and chunked", b);
	if (sa->bells != sb->bells)
		die("bells differ between whole and chunked", b);
}

/* Widths as mwt has them: in the C locale wcwidth() knows no wide character,
 * and none of the checks on them could fail. */
int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	if (!setlocale(LC_CTYPE, "C.UTF-8")) {
		fprintf(stderr, "fuzz_vt: no C.UTF-8 locale\n");
		abort();
	}
	return 0;
}

/* After output: no place a resize kept for the cursor outlives output to the
 * main screen (RESIZE.md K3) */
static void wrote(const struct vt *vt)
{
	if (vt->keep_cur && !(vt->mode & MODE_ALTSCREEN))
		abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;
	int cols = 1 + data[0] % 140, rows = 1 + data[1] % 60;
	data += 2;
	size -= 2;

	static struct said sa, sb;
	struct vt *a = mk(cols, rows, &sa);
	struct vt *b = mk(cols, rows, &sb);
	if (!a || !b)
		abort();
	a->allow_checksum = b->allow_checksum = true; /* DECRQCRA is parsed too */
	vt_write(a, (const char *)data, size);
	check(a);

	/* chunk lengths from the bytes themselves: deterministic, and the
	 * fuzzer can steer where sequences get cut */
	size_t off = 0;
	while (off < size) {
		size_t n = 1 + (data[off] * 7 + off) % 97;
		if (n > size - off)
			n = size - off;
		vt_write(b, (const char *)data + off, n);
		off += n;
		check(b);
		wrote(b);
	}
	compare(a, b, &sa, &sb);

	/* resizes taken from the last bytes, then the same text once more */
	for (size_t i = 0; i < 4 && i < size; i++) {
		uint8_t v = data[size - 1 - i];
		if (!vt_resize(b, 1 + v % 140, 1 + (v * 31) % 60))
			abort();
		check(b);
		/* and at times once more before anything is written, as a
		 * window dragged: narrower, then wider again */
		if ((v & 3) == 3 && !vt_resize(b, 1 + (v * 7) % 140, 1 + (v * 13) % 60))
			abort();
		check(b);
		vt_write(b, (const char *)data, size < 512 ? size : 512);
		check(b);
		if (size)
			wrote(b);
	}

	/* scroll through all of the history and back */
	vt_scroll_view(b, HIST);
	check(b);
	/* history lines keep the width they had, mwt.c clips them; reading
	 * every cell lets ASan see that len cells are really there */
	unsigned sum = 0;
	for (int r = 0; r < b->rows; r++) {
		int n;
		const struct cell *l = vt_view_line(b, r, &n);
		if (n < 0 || (!l && n))
			die("view line with a bad length", b);
		for (int x = 0; l && x < n; x++)
			sum += l[x].cp;
	}
	for (int64_t i = vt_abs_first(b); i < vt_abs_first(b) + b->hist_count; i++) {
		int n;
		const struct cell *l = vt_abs_line(b, i, &n);
		if (!l || n < 0)
			die("history line missing", b);
		for (int x = 0; x < n; x++)
			sum += l[x].cp;
	}
	if (sum == 0xdeadbeef)
		fputc(0, stderr); /* keep the reads */
	vt_scroll_view(b, -HIST);
	if (b->view != 0)
		die("view does not return to live", b);

	/* every key in every combination, in the modes the input left behind */
	for (int k = 0; k < VT_KEY_COUNT; k++)
		for (unsigned m = 0; m < 8; m++) {
			char key[16];
			size_t n = vt_key(a, (enum vt_key)k, m, key, sizeof key);
			if (n == 0 || n > sizeof key)
				die("a key without bytes", a);
		}

	vt_reset(b);
	check(b);
	vt_free(a);
	vt_free(b);
	return 0;
}

#ifdef FUZZ_STANDALONE
/* Each file once through LLVMFuzzerTestOneInput: a failed check aborts, and
 * the name printed before it says which file it was. */
int main(int argc, char **argv)
{
	int n = 0;
	LLVMFuzzerInitialize(&argc, &argv);
	for (int i = 1; i < argc; i++) {
		FILE *f = fopen(argv[i], "rb");
		if (!f) {
			perror(argv[i]);
			return 2;
		}
		static uint8_t buf[1 << 20];
		size_t len = fread(buf, 1, sizeof buf, f);
		bool err = ferror(f) || !feof(f);
		fclose(f);
		if (err) {
			fprintf(stderr, "%s: unreadable or over 1 MiB\n", argv[i]);
			return 2;
		}
		fprintf(stderr, "fuzz_regress: %s\n", argv[i]);
		LLVMFuzzerTestOneInput(buf, len);
		n++;
	}
	printf("fuzz_regress: %d inputs passed\n", n);
	return 0;
}
#endif
