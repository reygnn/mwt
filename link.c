/* link.c - see link.h */
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "paste.h"

struct lcell {
	uint32_t cp;
	uint16_t attr;
	int64_t abs;
	int col;
};

static bool url_char(uint32_t cp)
{
	if (cp <= ' ' || (cp >= 0x7f && cp <= 0xa0))
		return false; /* blanks, DEL, the C1 controls, no-break space */
	if (cp < 0x80)
		return !strchr("<>\"`{}|\\^", (int)cp);
	/* box drawing: a URL in tmux or mc ends at the frame next to it */
	if (cp >= 0x2500 && cp <= 0x259f)
		return false;
	/* other spaces, the angle brackets of math, CJK punctuation (。、「」) and
	 * the fullwidth forms of ASCII's (，）) */
	return !(cp >= 0x2000 && cp <= 0x200b) && !(cp >= 0x3000 && cp <= 0x303f) &&
	       !(cp >= 0xff01 && cp <= 0xff65) && cp != 0x27e8 && cp != 0x27e9;
}

/* The cells of line a that count: up to its wrap, when it goes on in the next. */
static int line_cells(const struct vt *vt, int64_t a, bool *wraps)
{
	int n, w = vt_abs_wraps(vt, a);
	vt_abs_line(vt, a, &n);
	*wraps = w > 0;
	return w ? w : n;
}

static bool alnum(uint32_t cp)
{
	return (cp >= '0' && cp <= '9') || ((cp | 0x20) >= 'a' && (cp | 0x20) <= 'z');
}

static const char *const schemes[] = { "https://", "http://", "ftp://", "file://", "mailto:" };

/* The length of the scheme that starts at c[i], or 0. */
static size_t scheme_at(const struct lcell *c, size_t i, size_t n)
{
	for (size_t s = 0; s < sizeof schemes / sizeof *schemes; s++) {
		size_t k = 0, len = strlen(schemes[s]);
		while (k < len && i + k < n) {
			uint32_t cp = c[i + k].cp;
			if (cp >= 'A' && cp <= 'Z')
				cp |= 0x20;
			if (cp != (unsigned char)schemes[s][k])
				break;
			k++;
		}
		if (k == len)
			return len;
	}
	return 0;
}

/* The last cell of a URL from c[a] to c[b]: punctuation that ends a sentence
 * goes, and a closing bracket the URL did not open, as in "(see http://x)".
 * The brackets counted once: a line of ))))) is trimmed in one pass. */
static size_t trim(const struct lcell *c, size_t a, size_t b)
{
	size_t open[2] = { 0, 0 }, close[2] = { 0, 0 };
	for (size_t i = a; i <= b; i++) {
		open[0] += c[i].cp == '(';
		close[0] += c[i].cp == ')';
		open[1] += c[i].cp == '[';
		close[1] += c[i].cp == ']';
	}
	while (b > a) {
		uint32_t cp = c[b].cp;
		int k = cp == ')' ? 0 : cp == ']' ? 1 : -1;
		if (k >= 0 && close[k] > open[k])
			close[k]--;
		else if (k < 0 && cp < 0x80 && strchr(".,:;!?'*", (int)cp))
			;
		else
			break;
		b--;
	}
	return b;
}

bool link_at(const struct vt *vt, int64_t abs, int col, struct link *l, char **url)
{
	int len;
	bool wr;
	const struct cell *row = vt_abs_line(vt, abs, &len);
	len = line_cells(vt, abs, &wr);
	if (!row || col < 0 || col >= len)
		return false;
	if (col > 0 && (row[col].attr & ATTR_WDUMMY))
		col--; /* the right half of a wide character is its left half's */

	/* the logical line around the pointer: lines that wrap into the next. The
	 * alternate screen does not go on from the history, which is the main
	 * screen's. */
	int64_t first = vt->mode & MODE_ALTSCREEN ? (int64_t)vt->scrolled : vt_abs_first(vt);
	int64_t a0 = abs, a1 = abs;
	long before = col, after = len - col;
	bool back = a0 > first && vt_abs_wraps(vt, a0 - 1);
	while (back && before < LINK_SPAN) {
		bool w;
		before += line_cells(vt, --a0, &w);
		back = a0 > first && vt_abs_wraps(vt, a0 - 1);
	}
	/* cut off by LINK_SPAN rather than at the line's start: what comes before
	 * is not seen, so neither is where a word or a URL begins */
	bool cut = back;
	for (bool fwd = wr; after < LINK_SPAN && fwd && vt_abs_line(vt, a1 + 1, &len);)
		after += line_cells(vt, ++a1, &fwd);
	struct lcell *c = malloc(((size_t)before + (size_t)after) * sizeof *c);
	if (!c)
		return false;
	size_t n = 0, p = SIZE_MAX;
	for (int64_t a = a0; a <= a1; a++) {
		const struct cell *r = vt_abs_line(vt, a, &len);
		bool w;
		len = line_cells(vt, a, &w);
		for (int k = 0; r && k < len; k++) {
			if (k > 0 && (r[k].attr & ATTR_WDUMMY))
				continue;
			if (a == abs && k == col)
				p = n;
			c[n++] = (struct lcell){ r[k].cp, r[k].attr, a, k };
		}
	}

	bool found = false;
	if (p != SIZE_MAX && url_char(c[p].cp) && !(c[p].attr & ATTR_INVISIBLE)) {
		/* the run of URL characters the pointer is in */
		size_t s = p, e = p;
		/* hidden text (SGR 8) ends it: what is opened is what is seen */
		while (s > 0 && url_char(c[s - 1].cp) && !(c[s - 1].attr & ATTR_INVISIBLE))
			s--;
		while (e + 1 < n && url_char(c[e + 1].cp) && !(c[e + 1].attr & ATTR_INVISIBLE))
			e++;
		/* the first scheme in the run that begins a word: one in its query
		 * (?u=https://...) is part of it, wherever the pointer is. A run cut
		 * at its start has none that can be trusted. */
		size_t start = 0, slen = 0;
		for (size_t i = s; i <= p && !slen && !(s == 0 && cut); i++) {
			size_t k = scheme_at(c, i, e + 1);
			if (k && (i == s || !alnum(c[i - 1].cp))) {
				start = i;
				slen = k;
			}
		}
		if (slen) {
			size_t end = trim(c, start, e);
			found = end >= start + slen && p <= end;
			if (found) {
				*l = (struct link){ c[start].abs, c[end].abs, c[start].col, c[end].col };
				if (url) {
					char *buf = NULL;
					size_t blen = 0, cap = 0;
					for (size_t i = start; found && i <= end; i++)
						found = put_utf8(&buf, &blen, &cap, c[i].cp);
					if (found)
						found = put_utf8(&buf, &blen, &cap, 0);
					if (found)
						*url = buf;
					else
						free(buf);
				}
			}
		}
	}
	free(c);
	return found;
}

bool link_same(const struct link *a, const struct link *b)
{
	return a->ba == b->ba && a->bc == b->bc && a->ea == b->ea && a->ec == b->ec;
}

int link_argv(const char *spec, const char *url, char *buf, size_t size, char **argv,
	      int max)
{
	size_t n = strlen(spec);
	if (n >= size)
		return 0;
	memcpy(buf, spec, n + 1);
	int argc = 0;
	for (char *s = buf; *s;) {
		while (*s == ' ' || *s == '\t')
			*s++ = '\0';
		if (!*s)
			break;
		if (argc + 2 >= max)
			return 0; /* room for url and the NULL */
		argv[argc++] = s;
		while (*s && *s != ' ' && *s != '\t')
			s++;
	}
	if (!argc)
		return 0;
	argv[argc++] = (char *)url;
	argv[argc] = NULL;
	return argc;
}
