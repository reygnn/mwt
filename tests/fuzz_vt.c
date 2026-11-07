/* fuzz_vt.c - coverage-guided fuzzing of the VT core with libFuzzer.
 *
 * The first two bytes pick the grid (1..140 x 1..60), the rest goes into two
 * terminals: one gets it in a single write, the other in chunks whose lengths
 * come from the input itself. Both must end in the same state, and after every
 * write the grid must be consistent (see check()). Then the second terminal is
 * resized a few times, the view scrolled through the history and the terminal
 * reset, each followed by the same checks. Sanitizers catch the rest.
 *
 *   make fuzz                     ten minutes, corpus in tests/fuzz-corpus
 *   make fuzz FUZZ_TIME=3600      one hour
 *   ./fuzz_vt crash-...           replay one finding
 *
 * Needs clang. The corpus starts from tests/vt100 if `make replay` fetched it. */
#define _XOPEN_SOURCE 700
#include "vt.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HIST 64

static void on_write(void *ud, const char *b, size_t n)
{
	(void)ud;
	(void)b;
	(void)n;
}

static void on_bell(void *ud)
{
	(void)ud;
}

static const struct vt_callbacks cb = { .write = on_write, .bell = on_bell };

static void die(const char *what, const struct vt *vt)
{
	fprintf(stderr, "fuzz_vt: %s (grid %dx%d, cursor %d,%d, region %d-%d)\n",
		what, vt->cols, vt->rows, vt->c.x, vt->c.y, vt->top, vt->bot);
	abort();
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
	struct cell **screens[2] = { vt->pri, vt->alt };
	for (int s = 0; s < 2; s++)
		for (int r = 0; r < vt->rows; r++) {
			const struct cell *l = screens[s][r];
			for (int x = 0; x < vt->cols; x++) {
				bool wide = l[x].attr & ATTR_WIDE;
				bool dummy = l[x].attr & ATTR_WDUMMY;
				if (wide && (x + 1 >= vt->cols || !(l[x + 1].attr & ATTR_WDUMMY)))
					die("wide cell without right half", vt);
				if (dummy && (x == 0 || !(l[x - 1].attr & ATTR_WIDE)))
					die("right half without wide cell", vt);
				if (l[x].cp >= 0x80 && l[x].cp < 0xa0)
					die("C1 control stored as text", vt);
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

static void compare(const struct vt *a, const struct vt *b)
{
	if (a->mode != b->mode)
		die("modes differ between whole and chunked", b);
	if (a->c.x != b->c.x || a->c.y != b->c.y || a->c.wrapnext != b->c.wrapnext ||
	    !same_cells(&a->c.tmpl, &b->c.tmpl, 1))
		die("cursor differs between whole and chunked", b);
	if (a->top != b->top || a->bot != b->bot)
		die("scroll region differs between whole and chunked", b);
	for (int r = 0; r < a->rows; r++)
		if (!same_cells(a->pri[r], b->pri[r], a->cols) ||
		    !same_cells(a->alt[r], b->alt[r], a->cols))
			die("screen differs between whole and chunked", b);
	if (a->hist_count != b->hist_count)
		die("history differs between whole and chunked", b);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	if (size < 2)
		return 0;
	int cols = 1 + data[0] % 140, rows = 1 + data[1] % 60;
	data += 2;
	size -= 2;

	struct vt *a = vt_new(cols, rows, HIST, &cb);
	struct vt *b = vt_new(cols, rows, HIST, &cb);
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
	}
	compare(a, b);

	/* resizes taken from the last bytes, then the same text once more */
	for (size_t i = 0; i < 4 && i < size; i++) {
		uint8_t v = data[size - 1 - i];
		if (!vt_resize(b, 1 + v % 140, 1 + (v * 31) % 60))
			abort();
		check(b);
		vt_write(b, (const char *)data, size < 512 ? size : 512);
		check(b);
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

	vt_reset(b);
	check(b);
	vt_free(a);
	vt_free(b);
	return 0;
}
