/* tests/font_oom.c - font.c when a strdup fails: each one in turn, once, in
 * font_open and in the fallback faces font_glyph loads. Linked with
 * --wrap=strdup and sanitizers (make font_oom). A face without its path once got
 * into find_fallback, whose strcmp then read NULL; that takes a fallback face
 * that has the character, so this runs in the image of `make fonts`. */
#include "font.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *__real_strdup(const char *s);

static int calls, fail_at = -1;

char *__wrap_strdup(const char *s)
{
	if (calls++ == fail_at)
		return NULL;
	return __real_strdup(s);
}

/* Latin, CJK, Hangul, an emoji, Devanagari, Thai: fallback faces where the
 * system has fonts for them, in all four styles */
static const uint32_t cps[] = { 'A', 0x6f22, 0xd55c, 0x1f642, 0x0905, 0x0e01 };

/* false when font_open gave up, which is the answer to a failure in it */
static bool run(const char *spec)
{
	struct font *f = font_open(spec, 0, 1);
	if (!f)
		return false;
	for (size_t i = 0; i < sizeof cps / sizeof *cps; i++)
		for (int s = 0; s < 4; s++)
			font_glyph(f, cps[i], s);
	font_close(f);
	return true;
}

int main(int argc, char **argv)
{
	const char *spec = argc > 1 ? argv[1] : "monospace:size=11";
	calls = 0;
	fail_at = -1;
	if (!run(spec)) {
		fprintf(stderr, "font_oom: no font for '%s'\n", spec);
		return 1;
	}
	int total = calls, refused = 0;
	if (total < 5) {
		fprintf(stderr, "font_oom: only %d strdups, no fallback face was loaded\n", total);
		return 1;
	}
	for (int k = 0; k < total; k++) {
		calls = 0;
		fail_at = k;
		refused += !run(spec);
	}
	printf("font_oom: each of %d strdups failed once, font_open gave up %d times, "
	       "nothing broke\n", total, refused);
	return 0;
}
