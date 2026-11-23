/* paste.h - what a paste becomes on its way to the program, and the clipboard's
 * types, without Wayland: mwt.c reads and writes the pipes, test_paste checks
 * the rest headless. */
#ifndef PASTE_H
#define PASTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Length of the valid UTF-8 sequence at s, n bytes there, or 0: no overlongs,
 * no surrogates, nothing past U+10FFFF. */
int utf8_seq(const unsigned char *s, size_t n);

/* cp appended to buf as UTF-8, buf grown as needed; false when out of memory,
 * and then buf is as it was. */
bool put_utf8(char **buf, size_t *len, size_t *cap, uint32_t cp);

enum { PASTE_OK, PASTE_NOMEM, PASTE_TOO_LARGE, PASTE_MORE };

/* Reads what a source sends from fd into buf (len used, cap allocated), at most
 * turn bytes in this call: PASTE_MORE when the fd has no more now or the turn is
 * used, PASTE_OK at its end (EOF or an error), PASTE_TOO_LARGE once more than max
 * bytes came, PASTE_NOMEM when buf could not grow. A paste of max bytes fits. */
int paste_read(int fd, char **buf, size_t *len, size_t *cap, size_t max, size_t turn);

/* What a source sent, as UTF-8 without C1 controls. A byte that is not part of
 * valid UTF-8 is taken as the Latin-1 character of its value, so nothing pasted
 * is lost; it grows to two bytes. More than max bytes of it is PASTE_TOO_LARGE.
 * On PASTE_OK *out is NULL for no text, and the caller frees it. */
int paste_decode(const char *in, size_t n, size_t max, char **out, size_t *len);

/* Text as it goes to the program, written to out, which takes n + 12 bytes: of
 * the controls only tab and CR stay, a line feed becomes CR, CR LF one CR. With
 * bracketed paste between ESC [ 200 ~ and ESC [ 201 ~. The count written. */
size_t paste_filter(const char *s, size_t n, bool bracketed, char *out);

/* The paste brackets in what has gone to the pty, which a write may cut
 * anywhere: seen is 0 to 4 for that much of "\033[20", 5 for "\033[200" and 6
 * for "\033[201". Pasted text holds no ESC, so nothing else looks alike. */
struct brackets {
	int seen;
	bool open; /* an opening bracket went out, its closing not */
};
void track_brackets(struct brackets *b, const char *s, size_t n);

/* The text types mwt offers and takes, best first, all of them UTF-8. STRING,
 * X11's Latin-1, is neither offered nor taken: who has it has UTF-8 as well. */
#define MIME_COUNT 4
extern const char *const mimes[MIME_COUNT];

/* Its place in mimes, MIME_COUNT for one that is not there. */
int mime_rank(const char *mime);

#endif
