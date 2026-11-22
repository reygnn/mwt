/* link.c - see link.h */
#include <stdlib.h>
#include <string.h>

#include "link.h"
#include "paste.h"

struct lcell {
	uint32_t cp;
	int64_t abs;
	int col;
};

static bool wraps(const struct vt *vt, int64_t abs)
{
	int n;
	const struct cell *l = vt_abs_line(vt, abs, &n);
	return l && n > 0 && (l[n - 1].attr & ATTR_WRAP);
}

static bool url_char(uint32_t cp)
{
	if (cp <= ' ' || (cp >= 0x7f && cp <= 0xa0))
		return false; /* blanks, DEL, the C1 controls, no-break space */
	if (cp < 0x80)
		return !strchr("<>\"`{}|\\^", (int)cp);
	/* box drawing: a URL in tmux or mc ends at the frame next to it */
	if (cp >= 0x2500 && cp <= 0x259f)
		return false;
	/* other spaces, and the angle brackets of math */
	return !(cp >= 0x2000 && cp <= 0x200b) && cp != 0x3000 && cp != 0x27e8 &&
	       cp != 0x27e9;
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

static size_t count(const struct lcell *c, size_t a, size_t b, uint32_t cp)
{
	size_t n = 0;
	for (size_t i = a; i <= b; i++)
		n += c[i].cp == cp;
	return n;
}

/* The last cell of a URL from c[a] to c[b]: punctuation that ends a sentence
 * goes, and a closing bracket the URL did not open, as in "(see http://x)". */
static size_t trim(const struct lcell *c, size_t a, size_t b)
{
	while (b > a) {
		uint32_t cp = c[b].cp;
		if (cp < 0x80 && strchr(".,:;!?'*", (int)cp))
			b--;
		else if (cp == ')' && count(c, a, b, ')') > count(c, a, b, '('))
			b--;
		else if (cp == ']' && count(c, a, b, ']') > count(c, a, b, '['))
			b--;
		else
			break;
	}
	return b;
}

bool link_at(const struct vt *vt, int64_t abs, int col, struct link *l, char **url)
{
	int len;
	const struct cell *row = vt_abs_line(vt, abs, &len);
	if (!row || col < 0 || col >= len)
		return false;
	if (col > 0 && (row[col].attr & ATTR_WDUMMY))
		col--; /* the right half of a wide character is its left half's */

	/* the logical line around the pointer: lines that wrap into the next */
	int64_t first = vt_abs_first(vt), a0 = abs, a1 = abs;
	long before = col, after = len - col;
	while (a0 > first && before < LINK_SPAN && wraps(vt, a0 - 1)) {
		int n;
		vt_abs_line(vt, --a0, &n);
		before += n;
	}
	while (after < LINK_SPAN && wraps(vt, a1) && vt_abs_line(vt, a1 + 1, &len)) {
		a1++;
		after += len;
	}
	struct lcell *c = malloc(((size_t)before + (size_t)after) * sizeof *c);
	if (!c)
		return false;
	size_t n = 0, p = 0;
	for (int64_t a = a0; a <= a1; a++) {
		const struct cell *r = vt_abs_line(vt, a, &len);
		for (int k = 0; r && k < len; k++) {
			if (k > 0 && (r[k].attr & ATTR_WDUMMY))
				continue;
			if (a == abs && k == col)
				p = n;
			c[n++] = (struct lcell){ r[k].cp, a, k };
		}
	}

	bool found = false;
	if (url_char(c[p].cp)) {
		/* the run of URL characters the pointer is in */
		size_t s = p, e = p;
		while (s > 0 && url_char(c[s - 1].cp))
			s--;
		while (e + 1 < n && url_char(c[e + 1].cp))
			e++;
		/* the last scheme at or before the pointer that begins a word */
		size_t start = 0, slen = 0;
		for (size_t i = s; i <= p; i++) {
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
