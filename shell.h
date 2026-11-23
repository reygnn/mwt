/* shell.h - the shell on its pty, without Wayland: what waits to be written to
 * it, and the child itself. mwt.c polls, test_shell checks it with real ptys,
 * pipes and processes. */
#ifndef SHELL_H
#define SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "paste.h"

/* An emptied buffer larger than this is given back. */
#define OUT_KEEP (64 * 1024)

/* What waits for the pty: len bytes from off. */
struct out {
	char *buf;
	size_t off, len, cap;
	size_t reply_len;         /* of those, at most this many are replies */
	struct brackets brackets; /* the paste brackets in what went out */
};

/* Nothing waits any more. */
void out_empty(struct out *o);

/* As much as fd takes now; the rest waits for the next. An error other than
 * EAGAIN drops what waits: the program is gone. */
void out_flush(struct out *o, int fd);

/* s appended and as much as fd takes written; false when out of memory, and
 * then s is dropped. Nothing with fd < 0. */
bool out_write(struct out *o, int fd, const char *s, size_t n);

/* A reply to the program: dropped while more than max replies wait, a program
 * that asks and does not read is not waiting for the answer. Only replies
 * count, behind a large paste the program may well read on. False when out of
 * memory. */
bool out_reply(struct out *o, int fd, const char *s, size_t n, size_t max);

/* A turn of reading what the program wrote: into buf, cap at a time, each piece
 * to got(); at most max bytes or ms milliseconds, whichever first, but always one
 * read. A short read waits pause_us before the next: the pty passes output on as
 * it comes, and yes gave 16 B a read, read() most of the CPU. */
struct shell_turn {
	char *buf;
	size_t cap, max;
	int ms, pause_us;
	int stop_fd; /* readable, it ends the turn: frames and keys wait no longer */
	void (*got)(void *ud, const char *s, size_t n);
	void *ud;
};

/* A read shorter than this is short. */
#define SHELL_SHORT_READ 1024

/* Reads a turn from fd and returns the bytes read; *gone at EOF or EIO, when the
 * program has gone. */
size_t shell_read(int fd, const struct shell_turn *turn, bool *gone);

/* Whether c, typed on the pty fd, raises a signal: ISIG on and c the INTR, QUIT
 * or SUSP character. */
bool shell_signal_char(int fd, unsigned char c);

/* Whether c, typed, must drop what waits for the program: something does, in o
 * or as a paste still being read (pasting), and c raises a signal. Only then is
 * the terminal asked, not at every key. */
bool shell_signal_drops(const struct out *o, bool pasting, int fd, unsigned char c);

/* For a key that raises a signal: what waits goes, also what the kernel holds
 * for the program unread, and a paste bracket that went out open is closed. */
void out_drop(struct out *o, int fd);

/* The shell: its pid, 0 once it is gone, and a pidfd or -1. */
struct child {
	pid_t pid;
	int fd;
};

/* argv, or $SHELL, the passwd entry's or /bin/sh, on a new pty of cols x rows,
 * its master nonblocking and close-on-exec in *pty. The child gets TERM=term,
 * COLORTERM=truecolor, no LINES or COLUMNS, default signals, none blocked, and
 * no file descriptor but 0 to 2; the pty has IUTF8. False when there is no pty
 * or no fork, with errno set and nothing left open. */
bool child_spawn(struct child *c, int *pty, char **argv, const char *term, int cols,
		 int rows);

/* Whether the shell is gone: reaped here, or already (ECHILD). Then pid is 0,
 * so no signal goes to its pid, which may be someone else's by now. */
bool child_gone(struct child *c);

/* Reaps the child if it ends within ms. */
void child_wait(struct child *c, int ms);

/* Closes the pty, which hangs up the session, sends SIGHUP and waits ms for the
 * child. No SIGKILL: what ignores SIGHUP (nohup) or takes longer has a reason. */
void child_hangup(struct child *c, int *pty, int ms);

/* argv[0] started with argv in a session of its own, for a link: with
 * XDG_ACTIVATION_TOKEN and DESKTOP_STARTUP_ID set to token, or neither when
 * token is NULL (mwt's own do not reach it), stdin and stdout on /dev/null,
 * mwt's stderr, default signals, none blocked, and no other file descriptor.
 * Through a child that ends at once and is reaped here, so mwt has no zombie
 * and the shell's pid stays the only child it waits for. False, with errno set,
 * when the pipe or either fork fails, the child between dies before its fork
 * (ECHILD), or the opener cannot be run (ENOENT for one that is not there): the
 * errno of its exec, or of moving the pipe, comes back through a pipe the exec
 * closes, waited for up to a second. */
bool opener_spawn(char *const argv[], const char *token);

#endif
