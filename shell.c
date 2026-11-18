/* shell.c - see shell.h */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

void out_empty(struct out *o)
{
	o->off = o->len = o->reply_len = 0;
	if (o->cap > OUT_KEEP) { /* a buffer a large paste grew */
		free(o->buf);
		o->buf = NULL;
		o->cap = 0;
	}
}

/* The pty takes a few KiB a write. What it took is skipped, not moved: moving
 * the rest after each write copied a 17 MB paste 22 GB over. */
void out_flush(struct out *o, int fd)
{
	while (o->len) {
		ssize_t n = write(fd, o->buf + o->off, o->len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno != EAGAIN)
				out_empty(o);
			return;
		}
		track_brackets(&o->brackets, o->buf + o->off, (size_t)n);
		o->off += (size_t)n;
		o->len -= (size_t)n;
		o->reply_len = MIN(o->reply_len, o->len);
	}
	out_empty(o);
}

bool out_write(struct out *o, int fd, const char *s, size_t n)
{
	if (fd < 0 || !n)
		return true;
	if (o->off + o->len + n > o->cap) {
		/* moved to the front only when what waits is no more than what was
		 * written, so each byte is moved a bounded number of times */
		if (o->off && o->off >= o->len) {
			memmove(o->buf, o->buf + o->off, o->len);
			o->off = 0;
		}
		if (o->off + o->len + n > o->cap) {
			size_t cap = MAX(o->cap * 2, o->off + o->len + n + 4096);
			char *p = realloc(o->buf, cap);
			if (!p)
				return false;
			o->buf = p;
			o->cap = cap;
		}
	}
	memcpy(o->buf + o->off + o->len, s, n);
	o->len += n;
	out_flush(o, fd);
	return true;
}

bool out_reply(struct out *o, int fd, const char *s, size_t n, size_t max)
{
	if (o->reply_len > max)
		return true;
	size_t before = o->len;
	if (!out_write(o, fd, s, n))
		return false;
	if (o->len > before)
		o->reply_len += MIN(n, o->len - before);
	return true;
}

bool shell_signal_char(int fd, unsigned char c)
{
	struct termios tm;
	return fd >= 0 && c && tcgetattr(fd, &tm) == 0 && (tm.c_lflag & ISIG) &&
	       (c == tm.c_cc[VINTR] || c == tm.c_cc[VQUIT] || c == tm.c_cc[VSUSP]);
}

/*
 * A key that raises a signal must not wait behind a paste the program does not
 * read, here or in the kernel, whose line buffer takes no more once it is full.
 * TCSAFLUSH on the master flushes the slave's input. A bracket that went out
 * open gets closed; one still waiting here goes with the rest, and the program
 * never sees it, so it needs no closing. What went out but was flushed in the
 * kernel unread cannot be told apart, and gets the closing too: a stray one
 * costs less than one missing, which would leave the program pasting whatever
 * is typed next.
 */
void out_drop(struct out *o, int fd)
{
	struct termios tm;
	if (!o->len || tcgetattr(fd, &tm) < 0)
		return;
	out_empty(o);
	tcflush(fd, TCOFLUSH);
	tcsetattr(fd, TCSAFLUSH, &tm);
	/* a bracket cut off halfway is ended by the ESC of the next */
	o->brackets.seen = 0;
	if (o->brackets.open)
		out_write(o, fd, "\033[201~", 6);
}

bool child_spawn(struct child *c, int *pty, char **argv, const char *term, int cols,
		 int rows)
{
	struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
	c->pid = forkpty(pty, NULL, NULL, &ws);
	c->fd = -1;
	if (c->pid < 0)
		return false;
	if (c->pid == 0) {
		setenv("TERM", term, 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");
		/* what mwt ignores or was started ignoring is not for the shell:
		 * `mwt &` from sh ignores SIGINT and SIGQUIT; nor what the program
		 * that started mwt had blocked */
		for (int sig = 1; sig < NSIG; sig++)
			signal(sig, SIG_DFL);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		/* nor what mwt was given open: what mwt opens is CLOEXEC, an fd
		 * its parent left open would reach the shell and all it starts */
		close_range(3, ~0U, 0);
		if (argv && argv[0]) {
			execvp(argv[0], argv);
			perror(argv[0]);
		} else {
			const char *sh = getenv("SHELL");
			struct passwd *pw = getpwuid(getuid());
			if (!sh || !*sh)
				sh = pw && pw->pw_shell && *pw->pw_shell ? pw->pw_shell : "/bin/sh";
			execl(sh, sh, (char *)NULL);
			perror(sh);
		}
		_exit(127);
	}
	fcntl(*pty, F_SETFL, fcntl(*pty, F_GETFL) | O_NONBLOCK);
	fcntl(*pty, F_SETFD, FD_CLOEXEC);
	c->fd = (int)syscall(SYS_pidfd_open, c->pid, 0);
	return true;
}

bool child_gone(struct child *c)
{
	if (c->pid <= 0)
		return true;
	pid_t r = waitpid(c->pid, NULL, WNOHANG);
	if (r == c->pid || (r < 0 && errno == ECHILD)) {
		c->pid = 0;
		return true;
	}
	return false;
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void child_wait(struct child *c, int ms)
{
	int64_t end = now_ms() + ms;
	for (;;) {
		if (child_gone(c))
			return;
		int64_t left = end - now_ms();
		if (left <= 0)
			return;
		/* without a pidfd (kernel < 5.3) this polls every 20 ms */
		struct pollfd p = { c->fd, POLLIN, 0 };
		poll(&p, 1, c->fd >= 0 ? (int)left : (int)MIN(left, 20));
	}
}

void child_hangup(struct child *c, int *pty, int ms)
{
	if (*pty >= 0) {
		close(*pty);
		*pty = -1;
	}
	if (child_gone(c)) /* no SIGHUP to a pid that may be reused */
		return;
	kill(c->pid, SIGHUP);
	child_wait(c, ms);
}
