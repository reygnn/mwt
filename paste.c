/* paste.c - see paste.h */
#include <stdlib.h>
#include <string.h>

#include "paste.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

int utf8_seq(const unsigned char *s, size_t n)
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

bool put_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp)
{
	if (*len + 5 > *cap) {
		size_t ncap = MAX(*cap * 2, 256);
		char *p = realloc(*buf, ncap);
		if (!p)
			return false;
		*buf = p;
		*cap = ncap;
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
	return true;
}

int paste_decode(const char *in, size_t n, bool latin1, size_t max, char **out,
		 size_t *len)
{
	char *buf = NULL;
	size_t cap = 0;
	*len = 0;
	for (size_t i = 0; i < n;) {
		const unsigned char *p = (const unsigned char *)in + i;
		int k = latin1 ? 0 : utf8_seq(p, n - i);
		uint32_t cp = *p;
		if (k > 1) {
			cp &= 0x3f >> (k - 1);
			for (int j = 1; j < k; j++)
				cp = cp << 6 | (p[j] & 0x3f);
		}
		i += k ? (size_t)k : 1;
		if ((cp < 0x80 || cp >= 0xa0) && !put_utf8(&buf, len, &cap, cp)) {
			free(buf);
			return PASTE_NOMEM;
		}
		if (*len > max) { /* Latin-1 grows */
			free(buf);
			return PASTE_TOO_LARGE;
		}
	}
	*out = buf;
	return PASTE_OK;
}

/* ESC would end the bracket, and ^C, ^Z, ^D, ^U, DEL and the like act in the
 * kernel's line discipline, bracketed paste or not: pasted text must not raise
 * a signal or edit. */
size_t paste_filter(const char *s, size_t n, bool bracketed, char *out)
{
	size_t o = 0;
	if (bracketed) {
		memcpy(out, "\033[200~", 6);
		o = 6;
	}
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\n') {
			if (i == 0 || s[i - 1] != '\r')
				out[o++] = '\r';
		} else if ((c >= 0x20 && c != 0x7f) || c == '\t' || c == '\r') {
			out[o++] = (char)c;
		}
	}
	if (bracketed) {
		memcpy(out + o, "\033[201~", 6);
		o += 6;
	}
	return o;
}

void track_brackets(struct brackets *b, const char *s, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		int k = b->seen;
		if (k < 4 && c == "\033[20"[k])
			b->seen = k + 1;
		else if (k == 4 && (c == '0' || c == '1'))
			b->seen = c == '0' ? 5 : 6;
		else {
			if (k >= 5 && c == '~')
				b->open = k == 5;
			b->seen = c == '\033';
		}
	}
}

const char *const mimes[MIME_COUNT] = {
	"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING",
};

int mime_rank(const char *mime)
{
	for (int i = 0; i < MIME_COUNT; i++)
		if (!strcmp(mime, mimes[i]))
			return i;
	return MIME_COUNT;
}

char *to_latin1(const char *s)
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
