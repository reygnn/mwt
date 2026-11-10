/* test_rc.c - mwtrc parsing, without a window: make check */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rc.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const struct rc_colors defaults = { 0xdcdccc, 0x1e1e1e, 0xdcdccc, 0x1e1e1e, 0xa0a0a0, {
	0x1e1e1e, 0xcc6666, 0x8fa86a, 0xd8b45e, 0x6c8fb8, 0xa87cb0, 0x6aa8a0, 0xc8c8c8,
	0x5a5a5a, 0xe08080, 0xa8c88a, 0xf0d080, 0x90b0d8, 0xc89ad0, 0x8ac8c0, 0xf4f4f4 } };

/* text as an mwtrc, for app_id; what stderr got lands in err */
static struct rc_colors run(const char *text, const char *app_id, char *err, size_t errsize)
{
	struct rc_colors c = defaults;
	FILE *f = fmemopen((void *)text, strlen(text), "r");
	FILE *e = tmpfile();
	fflush(stderr);
	int saved = dup(2);
	dup2(fileno(e), 2);
	rc_read(f, "mwtrc", app_id, &c);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	rewind(e);
	size_t n = fread(err, 1, errsize - 1, e);
	err[n] = '\0';
	fclose(e);
	fclose(f);
	return c;
}

static void test_color(void)
{
	uint32_t v = 1;
	CHECK(rc_color("#000040", &v) && v == 0x000040);
	CHECK(rc_color("#E5e5E5", &v) && v == 0xe5e5e5);
	CHECK(rc_color("#f80", &v) && v == 0xff8800);
	CHECK(rc_color("rgb:dc/dc/dc", &v) && v == 0xdcdcdc);
	CHECK(rc_color("rgb:f/0/8", &v) && v == 0xff0088);
	CHECK(rc_color("rgb:ffff/0000/8080", &v) && v == 0xff0080);
	v = 7;
	CHECK(!rc_color("", &v));
	CHECK(!rc_color("#00004", &v));
	CHECK(!rc_color("#0000400", &v));
	CHECK(!rc_color("#00004g", &v));
	CHECK(!rc_color("000040", &v));
	CHECK(!rc_color("rgb:dc/dc", &v));
	CHECK(!rc_color("rgb:dc/dc/dc/", &v));
	CHECK(!rc_color("rgb:12345/0/0", &v));
	CHECK(!rc_color("rgb:/0/0", &v));
	CHECK(!rc_color("indigo", &v));
	CHECK(v == 7);
}

static void test_read(void)
{
	char err[1024];
	struct rc_colors c;

	c = run("", "mwt", err, sizeof(err));
	CHECK(!memcmp(&c, &defaults, sizeof(c)) && !*err);

	/* the IRIS Indigo colors; the cursor follows the text */
	c = run("! winterm\n"
		"Mwt*foreground:   #e5e5e5\n"
		"  Mwt*background :#000040  \n", "mwt", err, sizeof(err));
	CHECK(c.fg == 0xe5e5e5 && c.bg == 0x000040 && c.cursor == 0xe5e5e5);
	CHECK(c.sel_fg == defaults.sel_fg && c.sel_bg == defaults.sel_bg && !*err);

	/* a cursorColor stays, wherever it is */
	c = run("Mwt*cursorColor: #ff0000\nMwt*foreground: #000040\n", "mwt", err, sizeof(err));
	CHECK(c.fg == 0x000040 && c.cursor == 0xff0000);
	/* no foreground: the cursor of config.h */
	c = run("Mwt*background: #e5e5e5\n", "mwt", err, sizeof(err));
	CHECK(c.cursor == defaults.cursor);

	/* the app_id wins over the global line, before or after it, ignoring case */
	const char *two = "Mwt*indigo*background: #dcdcdc\n"
			  "Mwt*background: #e5e5e5\n"
			  "mwt.Indigo.selectionBackground: #8080c0\n"
			  "MWT*selectionforeground: #ffffff\n";
	c = run(two, "INDIGO", err, sizeof(err));
	CHECK(c.bg == 0xdcdcdc && c.sel_bg == 0x8080c0 && c.sel_fg == 0xffffff && !*err);
	c = run(two, "mwt", err, sizeof(err));
	CHECK(c.bg == 0xe5e5e5 && c.sel_bg == defaults.sel_bg && c.sel_fg == 0xffffff);
	c = run(two, NULL, err, sizeof(err));
	CHECK(c.bg == 0xe5e5e5);

	/* later lines win; an app_id with dots */
	c = run("Mwt*background: #111111\nMwt*background: #222222\n"
		"Mwt*org.x.Term*background: #333333\n", "org.x.term", err, sizeof(err));
	CHECK(c.bg == 0x333333);
	c = run("Mwt*background: #111111\nMwt*background: #222222\n", "mwt", err, sizeof(err));
	CHECK(c.bg == 0x222222);

	/* bad lines are named and skipped, the good ones still count */
	c = run("Mwt*foreground #000040\n"
		"Xterm*background: #000000\n"
		"Mwt*: #000000\n"
		"Mwt*fontList: Sans\n"
		"Mwt*background: indigo\n"
		"Mwt*background: #e5e5e5\n", "mwt", err, sizeof(err));
	CHECK(c.bg == 0xe5e5e5 && c.fg == defaults.fg);
	CHECK(strstr(err, "mwtrc:1: expected resource: value"));
	CHECK(strstr(err, "mwtrc:2: expected Mwt*resource"));
	CHECK(strstr(err, "mwtrc:3: expected Mwt*resource"));
	CHECK(strstr(err, "mwtrc:4: unknown resource fontList"));
	CHECK(strstr(err, "mwtrc:5: background: not a color: \"indigo\""));
	CHECK(!strstr(err, "mwtrc:6"));
	/* another window's lines are not checked */
	c = run("Mwt*other*fontList: Sans\n", "mwt", err, sizeof(err));
	CHECK(!*err);

	/* the palette, by the names xterm has; the rest stays */
	c = run("Mwt*color0: #2e3750\n"
		"Mwt*Color4: #88bbe0\n"
		"Mwt*color15: rgb:e6/e8/ec\n"
		"Mwt*indigo*color4: #000080\n", "mwt", err, sizeof(err));
	CHECK(c.palette[0] == 0x2e3750 && c.palette[4] == 0x88bbe0 && c.palette[15] == 0xe6e8ec);
	CHECK(c.palette[1] == defaults.palette[1] && c.palette[14] == defaults.palette[14]);
	CHECK(c.fg == defaults.fg && c.cursor == defaults.cursor && !*err);
	c = run("Mwt*indigo*color4: #000080\nMwt*color4: #88bbe0\n", "indigo", err, sizeof(err));
	CHECK(c.palette[4] == 0x000080);
	/* only 0 to 15, written as xterm writes them */
	c = run("Mwt*color16: #000000\n"
		"Mwt*color04: #000000\n"
		"Mwt*color: #000000\n"
		"Mwt*color-1: #000000\n"
		"Mwt*color9: red\n", "mwt", err, sizeof(err));
	CHECK(!memcmp(&c, &defaults, sizeof(c)));
	CHECK(strstr(err, "mwtrc:1: unknown resource color16"));
	CHECK(strstr(err, "mwtrc:2: unknown resource color04"));
	CHECK(strstr(err, "mwtrc:3: unknown resource color\n"));
	CHECK(strstr(err, "mwtrc:4: unknown resource color-1"));
	CHECK(strstr(err, "mwtrc:5: color9: not a color: \"red\""));

	/* a line longer than any buffer, and no newline at the end */
	char *big = malloc(70000);
	memset(big, ' ', 70000);
	memcpy(big, "Mwt*background:", 15);
	strcpy(big + 70000 - 9, "#abcdef");
	c = run(big, "mwt", err, sizeof(err));
	CHECK(c.bg == 0xabcdef && !*err);
	free(big);
}

static void test_path(void)
{
	char buf[64];
	setenv("XDG_CONFIG_HOME", "/x", 1);
	setenv("HOME", "/h", 1);
	CHECK(rc_path(buf, sizeof(buf)) && !strcmp(buf, "/x/mwt/mwtrc"));
	setenv("XDG_CONFIG_HOME", "", 1);
	CHECK(rc_path(buf, sizeof(buf)) && !strcmp(buf, "/h/.config/mwt/mwtrc"));
	unsetenv("XDG_CONFIG_HOME");
	CHECK(rc_path(buf, sizeof(buf)) && !strcmp(buf, "/h/.config/mwt/mwtrc"));
	CHECK(!rc_path(buf, 10));
	unsetenv("HOME");
	CHECK(!rc_path(buf, sizeof(buf)));
}

int main(void)
{
	test_color();
	test_read();
	test_path();
	if (fails) {
		fprintf(stderr, "test_rc: %d failed\n", fails);
		return 1;
	}
	puts("test_rc: all passed");
	return 0;
}
