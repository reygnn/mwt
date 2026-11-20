/* tests/font_ttc.c - two faces of one font collection: a character only the
 * other face has comes from it, whichever index the font is at, also when a
 * style face is regular's again because its own did not load. font.c is built
 * in. Needs mwt-test.ttc from tests/mkttc.py, which tests/Containerfile.fonts
 * installs; make fonts runs it. Built with -Wl,--wrap=FT_New_Face: a face that
 * fails to load cannot be made real. */
#include "../font.c"

#include <stdio.h>

FT_Error __real_FT_New_Face(FT_Library, const char *, FT_Long, FT_Face *);

/* the call to FT_New_Face that fails, counted from 1; 0 for none */
static int calls, fail_call;

FT_Error __wrap_FT_New_Face(FT_Library lib, const char *path, FT_Long index, FT_Face *face)
{
	if (++calls == fail_call)
		return FT_Err_Cannot_Open_Resource;
	return __real_FT_New_Face(lib, path, index, face);
}

static int fails;

#define FAIL(...) do { fails++; printf("FAIL: " __VA_ARGS__); } while (0)

/* own: the character of the other face, at index other of the same file */
static void check(const char *family, uint32_t own, int other, int fail)
{
	char spec[128];
	snprintf(spec, sizeof spec, "%s:size=11", family);
	calls = 0;
	fail_call = fail;
	struct font *f = font_open(spec, 0, 1);
	fail_call = 0;
	if (!f) {
		FAIL("no font for '%s'\n", spec);
		return;
	}
	const char *path = f->style[STYLE_REGULAR].path;
	if (!strstr(path, "mwt-test.ttc")) {
		FAIL("'%s' is %s, not mwt-test.ttc\n", spec, path);
		font_close(f);
		return;
	}
	if (fail && f->style[STYLE_BOLD].index != f->style[STYLE_REGULAR].index)
		FAIL("'%s': bold made from regular's file at index %d, regular at %d\n", spec,
		     f->style[STYLE_BOLD].index, f->style[STYLE_REGULAR].index);
	for (int s = 0; s < 4; s++) {
		bool emb;
		struct face *fc = face_for(f, own, s, &emb);
		if (!fc || strcmp(fc->path, path) || fc->index != other)
			FAIL("'%s'%s: U+%04X in style %d from %s:%d, not index %d of the same file\n",
			     spec, fail ? " with bold not loaded" : "", (unsigned)own, s,
			     fc ? fc->path : "nothing", fc ? fc->index : -1, other);
		else if (!font_glyph(f, own, s, false)->w)
			FAIL("'%s': U+%04X in style %d not drawn\n", spec, (unsigned)own, s);
	}
	font_close(f);
}

int main(void)
{
	check("Mwt TTC A", 0x10FFF1, 1, 0);
	check("Mwt TTC B", 0x10FFF0, 0, 0);
	/* the second FT_New_Face is bold's: it is made from regular's file */
	check("Mwt TTC B", 0x10FFF0, 0, 2);
	if (fails) {
		printf("FAIL: %d failures\n", fails);
		return 1;
	}
	printf("font_ttc: each face of mwt-test.ttc draws what only the other has, also "
	       "with a bold face made from regular's file\n");
	return 0;
}
