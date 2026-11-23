/* test_shell.c - what waits for the pty, and the shell, with real pipes, ptys
 * and processes: make check */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "%s:%d: FAIL: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* realloc that fails once, the countdown-th call from now */
void *__real_realloc(void *p, size_t n);
static long countdown = -1;
void *__wrap_realloc(void *p, size_t n)
{
	if (countdown >= 0 && countdown-- == 0)
		return NULL;
	return __real_realloc(p, n);
}

/* kill as it is, with each signal sent noted: what no real process can show, a
 * signal to a pid that is no longer the shell's */
int __real_kill(pid_t pid, int sig);
struct sent { pid_t pid; int sig; };
static struct sent kills[16];
static int nkills;
int __wrap_kill(pid_t pid, int sig)
{
	if (sig && nkills < 16)
		kills[nkills++] = (struct sent){ pid, sig };
	return __real_kill(pid, sig);
}

/* fork that fails at the forks-th call from now, counted on in the child: 2
 * fails the second fork of opener_spawn, made in its child */
pid_t __real_fork(void);
static int forks;
static pid_t test_pid;
static bool kill_middle; /* opener_spawn's child dies of a signal after its fork */
pid_t __wrap_fork(void)
{
	if (forks > 0 && --forks == 0) {
		errno = ENOMEM; /* not EAGAIN, which opener_spawn falls back on */
		return -1;
	}
	pid_t pid = __real_fork();
	if (kill_middle && pid > 0 && getpid() != test_pid)
		raise(SIGKILL);
	return pid;
}

/* dup3 that fails, when asked; the child inherits the flag */
int __real_dup3(int old, int new, int flags);
static bool dup3_fails;
int __wrap_dup3(int old, int new, int flags)
{
	if (dup3_fails) {
		errno = EMFILE;
		return -1;
	}
	return __real_dup3(old, new, flags);
}

/* opendir as without /proc, when asked: hiding /proc for real needs a mount
 * namespace, which an unprivileged test may not get. The child inherits the
 * flag. */
static bool no_proc;
DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path)
{
	if (no_proc && !strcmp(path, "/proc/self/fd")) {
		errno = ENOENT;
		return NULL;
	}
	return __real_opendir(path);
}

static uint64_t rng = 0x853c49e6748fea9bu;
static unsigned rnd(unsigned n)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (unsigned)(rng % n);
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A pipe that takes one page at a time, its write end nonblocking: what a pty
 * does with a program that does not read. */
static void small_pipe(int fds[2])
{
	if (pipe2(fds, O_CLOEXEC) < 0) {
		perror("pipe2");
		exit(1);
	}
	fcntl(fds[1], F_SETPIPE_SZ, 4096);
	fcntl(fds[1], F_SETFL, O_NONBLOCK);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
}

/* up to max bytes of what fd holds now, appended to buf of size bytes */
static size_t drain_into(int fd, char *buf, size_t size, size_t *len, size_t max)
{
	size_t got = 0;
	max = MIN(max, size - *len);
	while (got < max) {
		ssize_t n = read(fd, buf + *len, MIN(max - got, 65536));
		if (n <= 0)
			break;
		*len += (size_t)n;
		got += (size_t)n;
	}
	return got;
}
#define drain(fd, buf, len, max) drain_into(fd, buf, sizeof buf, len, max)

/* out_empty keeps a small buffer for the next write, as mwt does for good */
static void out_free(struct out *o)
{
	out_empty(o);
	free(o->buf);
	*o = (struct out){ 0 };
}

/* the fds this process has open */
static int count_fds(void)
{
	DIR *d = opendir("/proc/self/fd");
	int n = 0;
	for (struct dirent *e; d && (e = readdir(d));)
		n += e->d_name[0] != '.';
	if (d)
		closedir(d);
	return n - 1; /* not the directory's own */
}

static int syscall_pidfd(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

static void test_write(void)
{
	int p[2];
	small_pipe(p);
	struct out o = { 0 };
	static char got[300000];
	size_t glen = 0;

	CHECK(out_write(&o, p[1], "hello", 5));
	CHECK(o.len == 0);
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == 5 && !memcmp(got, "hello", 5));
	/* what is written whole leaves a small buffer for the next */
	CHECK(o.buf && o.cap > 0 && o.cap <= OUT_KEEP);

	/* more than the pipe takes: the rest waits, in order */
	static char big[100000];
	for (size_t i = 0; i < sizeof big; i++)
		big[i] = (char)('a' + i % 26);
	glen = 0;
	CHECK(out_write(&o, p[1], big, sizeof big));
	CHECK(o.len == sizeof big - 4096);
	out_flush(&o, p[1]);
	CHECK(o.len == sizeof big - 4096);
	while (o.len) {
		drain(p[0], got, &glen, sizeof got);
		out_flush(&o, p[1]);
	}
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == sizeof big && !memcmp(got, big, sizeof big));
	/* emptied, a buffer that large is given back */
	CHECK(!o.buf && o.cap == 0 && o.reply_len == 0);

	/* nothing to write, or no pty */
	CHECK(out_write(&o, p[1], "x", 0) && o.len == 0);
	CHECK(out_write(&o, -1, "x", 1) && o.len == 0);
	out_free(&o);
	close(p[0]);
	close(p[1]);
}

/* What waits is moved to the front, not grown, when what went out is at least
 * as much as what waits. */
static void test_write_front(void)
{
	int p[2];
	small_pipe(p);
	struct out o = { 0 };
	static char a[10000], b[5000], got[20000];
	memset(a, 'a', sizeof a);
	memset(b, 'b', sizeof b);
	size_t glen = 0;
	out_write(&o, p[1], a, sizeof a);
	CHECK(o.off == 4096 && o.len == 5904 && o.cap == 14096);
	drain(p[0], got, &glen, 4096);
	out_flush(&o, p[1]);
	CHECK(o.off == 8192 && o.len == 1808);
	/* 8192 + 1808 + 5000 > 14096: moved, 1808 + 5000 fit */
	out_write(&o, p[1], b, sizeof b);
	CHECK(o.cap == 14096);
	while (o.len) {
		drain(p[0], got, &glen, sizeof got);
		out_flush(&o, p[1]);
	}
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == sizeof a + sizeof b && !memcmp(got, a, sizeof a) &&
	      !memcmp(got + sizeof a, b, sizeof b));
	out_free(&o);
	close(p[0]);
	close(p[1]);
}

/* Random writes into a reader that takes random amounts: every byte arrives
 * once, in order. The buffer grows only with off < len, so to below twice what
 * waits, and doubles: it stays within four times that, and the write. */
static void test_write_random(void)
{
	static char sent[1 << 22], got[1 << 22];
	long bad = 0;
	for (int round = 0; round < 40; round++) {
		int p[2];
		small_pipe(p);
		struct out o = { 0 };
		size_t slen = 0, glen = 0, most = 0;
		while (slen < sizeof sent - 20000) {
			size_t n = 1 + rnd(rnd(4) ? 300 : 20000);
			for (size_t i = 0; i < n; i++)
				sent[slen + i] = (char)rnd(256);
			if (!out_write(&o, p[1], sent + slen, n))
				bad++;
			slen += n;
			most = o.len > most ? o.len : most;
			if (o.cap > 4 * most + 2 * n + 8192)
				bad++;
			if (rnd(3) == 0) {
				drain(p[0], got, &glen, rnd(12000));
				out_flush(&o, p[1]);
			}
			if (rnd(50) == 0)
				break;
		}
		while (o.len) {
			drain(p[0], got, &glen, sizeof got);
			out_flush(&o, p[1]);
		}
		drain(p[0], got, &glen, sizeof got);
		if (glen != slen || memcmp(got, sent, slen))
			bad++;
		out_free(&o);
		close(p[0]);
		close(p[1]);
	}
	CHECK(bad == 0);
}

static void test_write_fail(void)
{
	/* the reader gone: what waits goes, the next write too */
	int p[2];
	small_pipe(p);
	struct out o = { 0 };
	static char big[20000];
	memset(big, 'z', sizeof big);
	out_write(&o, p[1], big, sizeof big);
	CHECK(o.len > 0);
	close(p[0]);
	out_flush(&o, p[1]);
	CHECK(o.len == 0 && o.off == 0);
	CHECK(out_write(&o, p[1], "x", 1) && o.len == 0);
	close(p[1]);
	out_free(&o);

	/* out of memory: false, and what waited is as it was */
	small_pipe(p);
	o = (struct out){ 0 };
	out_write(&o, p[1], big, sizeof big);
	size_t len = o.len, cap = o.cap;
	countdown = 0;
	CHECK(!out_write(&o, p[1], big, sizeof big));
	countdown = -1;
	CHECK(o.len == len && o.cap == cap);
	static char got[40000];
	size_t glen = 0;
	while (o.len) {
		drain(p[0], got, &glen, sizeof got);
		out_flush(&o, p[1]);
	}
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == sizeof big && !memcmp(got, big, sizeof big));
	out_free(&o);
	close(p[0]);
	close(p[1]);
}

/* the brackets count only what went out */
static void test_brackets_out(void)
{
	int p[2];
	small_pipe(p);
	struct out o = { 0 };
	static char paste[20006], got[40000];
	memcpy(paste, "\033[200~", 6);
	memset(paste + 6, 'x', sizeof paste - 6);
	size_t glen = 0;
	out_write(&o, p[1], paste, sizeof paste);
	CHECK(o.brackets.open);
	out_write(&o, p[1], "\033[201~", 6);
	CHECK(o.brackets.open); /* the closing one waits */
	while (o.len) {
		drain(p[0], got, &glen, sizeof got);
		out_flush(&o, p[1]);
	}
	CHECK(!o.brackets.open);
	drain(p[0], got, &glen, sizeof got);

	/* one write with both, the pipe takes only the first: open */
	static char both[8012];
	memcpy(both, "\033[200~", 6);
	memset(both + 6, 'y', 8000);
	memcpy(both + 8006, "\033[201~", 6);
	out_write(&o, p[1], both, sizeof both);
	CHECK(o.len == sizeof both - 4096 && o.brackets.open);
	glen = 0;
	while (o.len) {
		drain(p[0], got, &glen, sizeof got);
		out_flush(&o, p[1]);
	}
	CHECK(!o.brackets.open);
	drain(p[0], got, &glen, sizeof got);

	/* a full pipe: the opening bracket waits too, nothing is open */
	char fill[4096];
	memset(fill, 'f', sizeof fill);
	CHECK(write(p[1], fill, sizeof fill) == (ssize_t)sizeof fill);
	out_write(&o, p[1], paste, sizeof paste);
	CHECK(!o.brackets.open && o.len == sizeof paste);
	/* cut after "\033[20": seen 4 */
	glen = 0;
	drain(p[0], got, &glen, 4);
	out_flush(&o, p[1]);
	out_free(&o);
	close(p[0]);
	close(p[1]);
}

static void test_reply(void)
{
	int p[2];
	small_pipe(p);
	struct out o = { 0 };
	char fill[4096];
	memset(fill, 'f', sizeof fill);
	CHECK(write(p[1], fill, sizeof fill) == (ssize_t)sizeof fill);
	/* a program that does not read: replies up to the limit, then none */
	for (int i = 0; i < 5; i++)
		CHECK(out_reply(&o, p[1], "0123456789", 10, 25));
	CHECK(o.len == 30 && o.reply_len == 30);
	/* a paste goes on, and does not count */
	out_write(&o, p[1], "pasted", 6);
	CHECK(o.len == 36 && o.reply_len == 30);
	/* once the program reads, they go out and count no more */
	static char got[65536];
	size_t glen = 0;
	drain(p[0], got, &glen, sizeof got);
	out_flush(&o, p[1]);
	CHECK(o.len == 0 && o.reply_len == 0);
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == 4096 + 36 && !memcmp(got + 4096, "012345678901234567890123456789pasted", 36));
	CHECK(out_reply(&o, p[1], "after", 5, 25));
	glen = 0;
	drain(p[0], got, &glen, sizeof got);
	CHECK(glen == 5);

	/* a reply cut by the pipe counts what waits of it */
	static char big[10000];
	memset(big, 'r', sizeof big);
	CHECK(out_reply(&o, p[1], big, sizeof big, 1 << 20));
	CHECK(o.len == 10000 - 4096 && o.reply_len == o.len);
	/* what goes out of it brings the count down with it */
	glen = 0;
	drain(p[0], got, &glen, sizeof got);
	out_flush(&o, p[1]);
	CHECK(o.len == 10000 - 2 * 4096 && o.reply_len == o.len);
	drain(p[0], got, &glen, sizeof got);
	out_flush(&o, p[1]);
	CHECK(o.len == 0 && o.reply_len == 0);
	/* out of memory */
	glen = 0;
	drain(p[0], got, &glen, sizeof got);
	CHECK(write(p[1], fill, sizeof fill) == (ssize_t)sizeof fill);
	out_free(&o); /* no buffer: the reply needs one */
	countdown = 0;
	CHECK(!out_reply(&o, p[1], "abc", 3, 25));
	countdown = -1;
	CHECK(o.reply_len == 0);
	out_free(&o);
	close(p[0]);
	close(p[1]);
}

/* a pty whose slave is raw, but with ISIG, read without blocking */
static void raw_pty(int *m, int *s)
{
	if (openpty(m, s, NULL, NULL, NULL) < 0) {
		perror("openpty");
		exit(1);
	}
	struct termios tm;
	tcgetattr(*s, &tm);
	cfmakeraw(&tm);
	tm.c_lflag |= ISIG;
	tcsetattr(*s, TCSANOW, &tm);
	fcntl(*m, F_SETFL, O_NONBLOCK);
	fcntl(*s, F_SETFL, O_NONBLOCK);
}

/* Ctrl+C drops only what waits; with nothing, the terminal is not asked */
static void test_signal_drops(void)
{
	int m, s;
	if (openpty(&m, &s, NULL, NULL, NULL) < 0) {
		perror("openpty");
		exit(1);
	}
	struct out o = { 0 };
	CHECK(!shell_signal_drops(&o, false, m, 0x03));
	CHECK(shell_signal_drops(&o, true, m, 0x03));
	CHECK(!shell_signal_drops(&o, true, m, 'c'));
	o.len = 1;
	CHECK(shell_signal_drops(&o, false, m, 0x03));
	CHECK(!shell_signal_drops(&o, false, m, 'c'));
	close(s);
	close(m);
}

static void test_signal_char(void)
{
	int m, s;
	if (openpty(&m, &s, NULL, NULL, NULL) < 0) {
		perror("openpty");
		exit(1);
	}
	CHECK(shell_signal_char(m, 0x03));
	CHECK(shell_signal_char(m, 0x1c));
	CHECK(shell_signal_char(m, 0x1a));
	CHECK(!shell_signal_char(m, 'c'));
	CHECK(!shell_signal_char(m, 0x04)); /* EOF raises nothing */
	CHECK(!shell_signal_char(m, 0));
	CHECK(!shell_signal_char(-1, 0x03));
	/* as the program set it */
	struct termios tm;
	tcgetattr(s, &tm);
	tm.c_cc[VINTR] = 'x';
	tcsetattr(s, TCSANOW, &tm);
	CHECK(shell_signal_char(m, 'x'));
	CHECK(!shell_signal_char(m, 0x03));
	tm.c_lflag &= ~ISIG;
	tcsetattr(s, TCSANOW, &tm);
	CHECK(!shell_signal_char(m, 'x'));
	CHECK(!shell_signal_char(m, 0x1c));
	close(s);
	close(m);
	/* not a terminal */
	int p[2];
	small_pipe(p);
	CHECK(!shell_signal_char(p[1], 0x03));
	close(p[0]);
	close(p[1]);
	test_signal_drops();
}

static void test_drop(void)
{
	int m, s;
	raw_pty(&m, &s);
	struct out o = { 0 };
	static char paste[200006], got[300000];
	memcpy(paste, "\033[200~", 6);
	memset(paste + 6, 'p', sizeof paste - 6);
	size_t glen = 0;
	/* a paste the program does not read: the kernel holds some, mwt the rest */
	out_write(&o, m, paste, sizeof paste);
	CHECK(o.len > 0 && o.brackets.open);
	out_drop(&o, m);
	/* it all goes, the kernel's too: the program reads only the closing bracket */
	CHECK(o.len == 0 && !o.brackets.open);
	drain(s, got, &glen, sizeof got);
	CHECK(glen == 6 && !memcmp(got, "\033[201~", 6));

	/* the opening bracket never went out: no closing either */
	char fill[65536];
	memset(fill, 'f', sizeof fill);
	out_write(&o, m, fill, sizeof fill);
	CHECK(o.len > 0 && !o.brackets.open);
	out_write(&o, m, paste, sizeof paste);
	out_drop(&o, m);
	glen = 0;
	drain(s, got, &glen, sizeof got);
	CHECK(o.len == 0 && glen == 0);

	/* nothing waits here: what the kernel holds stays, the program reads it */
	out_write(&o, m, "typed", 5);
	CHECK(o.len == 0);
	out_drop(&o, m);
	glen = 0;
	drain(s, got, &glen, sizeof got);
	CHECK(glen == 5 && !memcmp(got, "typed", 5));

	/* a bracket cut off halfway is forgotten: the next one starts afresh */
	out_write(&o, m, fill, sizeof fill);
	CHECK(o.len > 0);
	o.brackets.seen = 3; /* "\033[2" went out last */
	out_drop(&o, m);
	CHECK(o.brackets.seen == 0);
	drain(s, got, &glen, sizeof got);
	out_free(&o);
	close(s);
	close(m);
}

static void test_gone(void)
{
	struct child c = { 0, -1 };
	CHECK(child_gone(&c));
	/* not our child: ECHILD, gone, and no signal can follow */
	c.pid = 1;
	CHECK(child_gone(&c) && c.pid == 0);

	pid_t pid = fork();
	if (pid == 0)
		_exit(0);
	c = (struct child){ pid, -1 };
	int64_t t0 = now_ms();
	child_wait(&c, 5000);
	CHECK(c.pid == 0 && now_ms() - t0 < 2000);

	/* one that runs: not gone, the wait ends after ms */
	pid = fork();
	if (pid == 0) {
		pause();
		_exit(0);
	}
	c = (struct child){ pid, syscall_pidfd(pid) };
	CHECK(!child_gone(&c) && c.pid == pid);
	t0 = now_ms();
	child_wait(&c, 200);
	int64_t took = now_ms() - t0;
	CHECK(c.pid == pid && took >= 190 && took < 2000);
	__real_kill(pid, SIGKILL);
	child_wait(&c, 5000);
	CHECK(c.pid == 0);
	close(c.fd);

	/* without a pidfd it is asked every 20 ms */
	pid = fork();
	if (pid == 0) {
		usleep(100000);
		_exit(0);
	}
	c = (struct child){ pid, -1 };
	t0 = now_ms();
	child_wait(&c, 5000);
	took = now_ms() - t0;
	CHECK(c.pid == 0 && took >= 90 && took < 1000);

	/* SIGCHLD ignored: the kernel reaped it, waitpid says ECHILD */
	signal(SIGCHLD, SIG_IGN);
	pid = fork();
	if (pid == 0)
		_exit(0);
	for (int i = 0; i < 500 && __real_kill(pid, 0) == 0; i++)
		usleep(2000);
	c = (struct child){ pid, -1 };
	CHECK(child_gone(&c) && c.pid == 0);
	signal(SIGCHLD, SIG_DFL);
}

/* the shell's output until it ends, and its exit status */
static int run(char **argv, const char *input, char *buf, size_t cap)
{
	struct child c;
	int pty;
	if (!child_spawn(&c, &pty, argv, "mwt-test", 80, 24)) {
		failures++;
		fprintf(stderr, "FAIL: child_spawn\n");
		return -1;
	}
	if (input)
		CHECK(write(pty, input, strlen(input)) == (ssize_t)strlen(input));
	size_t len = 0;
	for (int64_t end = now_ms() + 10000; now_ms() < end;) {
		struct pollfd p = { pty, POLLIN, 0 };
		poll(&p, 1, 100);
		ssize_t n = read(pty, buf + len, cap - 1 - len);
		if (n > 0)
			len += (size_t)n;
		else if (n < 0 && errno == EAGAIN)
			continue;
		else
			break; /* EIO: the slave is closed */
	}
	buf[len] = 0;
	close(pty);
	int status = 0;
	waitpid(c.pid, &status, 0);
	close(c.fd);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/* The fds check in a child that seccomp refuses close_range, as a kernel
 * before 5.9 or a container's filter does. Without /proc each fd up to the hard
 * limit, also past a soft limit below 50; then through /proc/self/fd, which a
 * hard limit below 50 does not stop either. */
static void no_close_range(char **fds, char *out, size_t cap)
{
	fflush(stderr);
	pid_t pid = fork();
	if (pid == 0) {
		failures = 0;
#ifdef __NR_close_range
		struct sock_filter f[] = {
			BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
			BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_close_range, 0, 1),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
			BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
		};
		struct sock_fprog prog = { sizeof f / sizeof f[0], f };
		CHECK(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0);
		CHECK(prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0);
		CHECK(syscall(__NR_close_range, 1000U, 1000U, 0U) < 0 && errno == ENOSYS);
#endif
		struct rlimit rl;
		CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
		struct rlimit low = { 20, rl.rlim_max };
		CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
		no_proc = true;
		CHECK(run(fds, NULL, out, cap) == 0);
		CHECK(!strcmp(out, "0, 1, 2\r\n"));
		no_proc = false;
		low.rlim_max = 20; /* for good, in this child only */
		CHECK(setrlimit(RLIMIT_NOFILE, &low) == 0);
		CHECK(run(fds, NULL, out, cap) == 0);
		CHECK(!strcmp(out, "0, 1, 2\r\n"));
		_exit(failures != 0);
	}
	int st = 0;
	CHECK(pid > 0 && waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
}

static char *cat_argv[] = { "cat", NULL };

static void test_spawn(void)
{
	static char out[8192];
	setenv("COLORTERM", "rgb", 1);
	setenv("LINES", "5", 1);
	setenv("COLUMNS", "7", 1);
	char *env[] = { "sh", "-c",
		"echo \"T=$TERM C=$COLORTERM L=${LINES-none} K=${COLUMNS-none}\"", NULL };
	CHECK(run(env, NULL, out, sizeof out) == 0);
	CHECK(strstr(out, "T=mwt-test C=truecolor L=none K=none"));
	unsetenv("LINES");
	unsetenv("COLUMNS");
	unsetenv("COLORTERM");

	char *size[] = { "stty", "size", NULL };
	CHECK(run(size, NULL, out, sizeof out) == 0);
	CHECK(strstr(out, "24 80"));

	/* what mwt ignores or blocks is not for the shell */
	signal(SIGINT, SIG_IGN);
	signal(SIGQUIT, SIG_IGN);
	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigaddset(&block, SIGUSR1);
	sigprocmask(SIG_BLOCK, &block, &old);
	char *sigs[] = { "grep", "^Sig\\(Ign\\|Blk\\)", "/proc/self/status", NULL };
	CHECK(run(sigs, NULL, out, sizeof out) == 0);
	CHECK(strstr(out, "SigBlk:\t0000000000000000"));
	CHECK(strstr(out, "SigIgn:\t0000000000000000"));
	sigprocmask(SIG_SETMASK, &old, NULL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);

	/* nor what it was given open, 3 and 50 here; ls run by the shell, not
	 * exec'd in its place nor in a pipe, so $$ is the shell's and holds no
	 * pipe while ls looks */
	int fd = open("/dev/null", O_RDONLY);
	CHECK(dup2(fd, 3) == 3 && dup2(fd, 50) == 50);
	if (fd != 3)
		close(fd);
	char *fds[] = { "sh", "-c", "ls -m /proc/$$/fd; exit", NULL };
	CHECK(run(fds, NULL, out, sizeof out) == 0);
	CHECK(!strcmp(out, "0, 1, 2\r\n"));
	no_close_range(fds, out, sizeof out);
	close(50);
	close(3);

	/* the pty is the shell's controlling terminal, in a session of its own */
	char *ctty[] = { "sh", "-c",
		"set -- $(cat /proc/$$/stat); [ \"$6\" = $$ ] && : </dev/tty && echo ctty-ok", NULL };
	CHECK(run(ctty, NULL, out, sizeof out) == 0);
	CHECK(strstr(out, "ctty-ok"));

	/* IUTF8: in canonical mode an erase takes the whole of a character, not
	 * its last byte; set before the first input arrives */
	char *line[] = { "sh", "-c", "read l; printf '<%s>' \"$l\" | od -An -tx1", NULL };
	CHECK(run(line, "a\xc3\xa4\x7f\xe2\x82\xac\x7f\r", out, sizeof out) == 0);
	CHECK(strstr(out, "3c 61 3e"));

	/* no pty, or no fork: false, errno for the message, nothing left open;
	 * for real, by the limits. As root RLIMIT_NPROC does not hold. */
	struct child f;
	int fpty = 7, n0 = count_fds(), lowest = 0;
	while (fcntl(lowest, F_GETFD) >= 0)
		lowest++;
	struct rlimit rl;
	CHECK(getrlimit(RLIMIT_NOFILE, &rl) == 0);
	struct rlimit tight = { (rlim_t)lowest, rl.rlim_max }; /* none free below it */
	CHECK(setrlimit(RLIMIT_NOFILE, &tight) == 0);
	errno = 0;
	bool made = child_spawn(&f, &fpty, cat_argv, "mwt-test", 80, 24);
	int e = errno;
	CHECK(setrlimit(RLIMIT_NOFILE, &rl) == 0);
	/* EMFILE; glibc 2.31 then tries BSD ptys and leaves ENOENT */
	CHECK(!made && fpty == -1 && f.pid == -1 && f.fd == -1 && e != 0);
	CHECK(count_fds() == n0);
	fflush(stderr);
	pid_t helper = fork();
	if (helper == 0) {
		failures = 0;
		if (getuid() != 0) {
			struct rlimit none = { 0, 0 };
			CHECK(setrlimit(RLIMIT_NPROC, &none) == 0);
			int m0 = count_fds();
			fpty = 7;
			errno = 0;
			made = child_spawn(&f, &fpty, cat_argv, "mwt-test", 80, 24);
			CHECK(!made && fpty == -1 && f.pid == -1 && errno == EAGAIN);
			CHECK(count_fds() == m0);
		}
		_exit(failures != 0);
	}
	int hs = 0;
	CHECK(waitpid(helper, &hs, 0) == helper && WIFEXITED(hs) && WEXITSTATUS(hs) == 0);

	/* a program that is not there */
	char *none[] = { "/nicht/da", NULL };
	CHECK(run(none, NULL, out, sizeof out) == 127);
	CHECK(strstr(out, "/nicht/da: No such file or directory"));

	/* no command: $SHELL, else the passwd entry's */
	setenv("SHELL", "/bin/sh", 1);
	CHECK(run(NULL, "echo sh-$((6*7))\rexit 3\r", out, sizeof out) == 3);
	CHECK(strstr(out, "sh-42"));
	setenv("SHELL", "", 1);
	CHECK(run(NULL, "echo pw-$((1+1))\rexit 4\r", out, sizeof out) == 4);
	CHECK(strstr(out, "pw-2"));
	unsetenv("SHELL");

	/* the master: nonblocking, not for what mwt starts later; a pidfd */
	struct child c;
	int pty;
	char *cat[] = { "cat", NULL };
	int before = count_fds();
	CHECK(child_spawn(&c, &pty, cat, "mwt-test", 80, 24));
	CHECK(count_fds() == before + 2); /* the master and the pidfd, not the slave */
	CHECK(fcntl(pty, F_GETFL) & O_NONBLOCK);
	CHECK(fcntl(pty, F_GETFD) & FD_CLOEXEC);
	CHECK(c.fd >= 0 && (fcntl(c.fd, F_GETFD) & FD_CLOEXEC));
	nkills = 0;
	child_hangup(&c, &pty, 5000);
	CHECK(pty == -1 && c.pid == 0);
	close(c.fd);
}

/* Whether the pty showed what, within 10 s: the child is past where it
 * prints it, rather than a pause that a loaded machine outlasts. */
static bool shown(int pty, const char *what)
{
	char buf[256];
	size_t len = 0;
	for (int64_t end = now_ms() + 10000; now_ms() < end;) {
		struct pollfd p = { pty, POLLIN, 0 };
		poll(&p, 1, 100);
		ssize_t n = read(pty, buf + len, sizeof buf - 1 - len);
		if (n > 0) {
			len += (size_t)n;
			buf[len] = 0;
			if (strstr(buf, what))
				return true;
			if (len == sizeof buf - 1)
				len = 0;
		} else if (n < 0 && errno != EAGAIN) {
			return false;
		}
	}
	return false;
}

static void test_hangup(void)
{
	/* the shell gets SIGHUP and ends: no wait */
	struct child c;
	int pty;
	char *sh[] = { "sh", "-c", "echo ready; sleep 30", NULL };
	CHECK(child_spawn(&c, &pty, sh, "mwt-test", 80, 24));
	pid_t pid = c.pid;
	CHECK(shown(pty, "ready"));
	nkills = 0;
	int64_t t0 = now_ms();
	child_hangup(&c, &pty, 5000);
	CHECK(c.pid == 0 && pty == -1 && now_ms() - t0 < 2000);
	/* the kernel's SIGHUP for the closed pty may have ended it first: then
	 * none from mwt. That mwt sends one shows the case after this. */
	CHECK(nkills <= 1 && (nkills == 0 || (kills[0].pid == pid && kills[0].sig == SIGHUP)));
	close(c.fd);

	/* one that ignores SIGHUP is waited for, then left: no SIGKILL */
	char *nohup[] = { "sh", "-c", "trap '' HUP; echo ready; exec sleep 30", NULL };
	CHECK(child_spawn(&c, &pty, nohup, "mwt-test", 80, 24));
	pid = c.pid;
	CHECK(shown(pty, "ready"));
	nkills = 0;
	t0 = now_ms();
	child_hangup(&c, &pty, 300);
	int64_t took = now_ms() - t0;
	CHECK(took >= 290 && took < 2000);
	CHECK(c.pid == pid && __real_kill(pid, 0) == 0);
	CHECK(nkills == 1 && kills[0].sig == SIGHUP);
	__real_kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	close(c.fd);

	/* Reaped already by the event loop, its pid may be someone else's: no
	 * signal at all. */
	char *quick[] = { "true", NULL };
	CHECK(child_spawn(&c, &pty, quick, "mwt-test", 80, 24));
	struct pollfd p = { c.fd, POLLIN, 0 };
	CHECK(poll(&p, 1, 5000) == 1);
	CHECK(child_gone(&c));
	nkills = 0;
	child_hangup(&c, &pty, 5000);
	CHECK(nkills == 0);
	close(c.fd);

	/* SIGCHLD ignored, the pidfd saw the end, the kernel reaped it */
	signal(SIGCHLD, SIG_IGN);
	CHECK(child_spawn(&c, &pty, quick, "mwt-test", 80, 24));
	p = (struct pollfd){ c.fd, POLLIN, 0 };
	CHECK(poll(&p, 1, 5000) == 1);
	nkills = 0;
	child_hangup(&c, &pty, 5000);
	CHECK(nkills == 0 && c.pid == 0);
	close(c.fd);
	signal(SIGCHLD, SIG_DFL);
}

static size_t got_calls, got_bytes;

static void count_got(void *ud, const char *s, size_t n)
{
	(void)ud; (void)s;
	got_calls++;
	got_bytes += n;
}

static int64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void wait_readable(int fd, int ms)
{
	struct pollfd p = { fd, POLLIN, 0 };
	poll(&p, 1, ms);
}

/* A turn of reading: a flood in small pieces is read in larger ones after a
 * pause, a readable stop fd ends it, EOF or EIO tell the program has gone */
static void test_read_turn(void)
{
	static char buf[65536];
	struct shell_turn turn = { buf, sizeof buf, 1 << 20, 100, 0, -1, count_got, NULL };
	int m, s;
	raw_pty(&m, &s);
	/* one short read: the turn waits once, then finds nothing */
	CHECK(write(s, "hello", 5) == 5);
	wait_readable(m, 5000);
	got_calls = got_bytes = 0;
	bool gone = false;
	turn.pause_us = 1000;
	CHECK(shell_read(m, &turn, &gone) == 5 && !gone && got_calls == 1);
	/* nothing there: no read is a turn's worth */
	CHECK(shell_read(m, &turn, &gone) == 0 && !gone);

	/* 16 bytes every 100 µs: each short read waits 2 ms, and the turn reads
	 * on for its 100 ms rather than ending at the first EAGAIN, also when the
	 * writer shares the reader's CPU */
	pid_t pid = fork();
	if (pid == 0) {
		fcntl(s, F_SETFL, 0);
		int64_t end = mono_us() + 300000;
		while (mono_us() < end) {
			if (write(s, "0123456789abcdef", 16) != 16)
				_exit(1);
			struct timespec ts = { 0, 100000 };
			nanosleep(&ts, NULL);
		}
		_exit(0);
	}
	wait_readable(m, 5000);
	turn.pause_us = 2000;
	got_calls = got_bytes = 0;
	shell_read(m, &turn, &gone);
	CHECK(!gone && got_calls > 2 && got_bytes >= 2048);
	if (got_bytes < 2048)
		fprintf(stderr, "  %zu reads, %zu bytes in the turn\n", got_calls, got_bytes);
	/* the stop fd readable, as the compositor's with a frame callback: the
	 * turn ends after one read, flood or not */
	int p[2];
	small_pipe(p);
	CHECK(write(p[1], "x", 1) == 1);
	turn.stop_fd = p[0];
	wait_readable(m, 5000);
	got_calls = 0;
	shell_read(m, &turn, &gone);
	CHECK(got_calls == 1);
	turn.stop_fd = -1;
	close(p[0]);
	close(p[1]);
	/* drain, or the writer blocks on a full pty and never ends */
	turn.pause_us = 0;
	while (waitpid(pid, NULL, WNOHANG) == 0) {
		shell_read(m, &turn, &gone);
		wait_readable(m, 50);
	}
	while (shell_read(m, &turn, &gone) > 0)
		;
	/* the slave closed: EIO, the program has gone */
	close(s);
	CHECK(shell_read(m, &turn, &gone) == 0 && gone);
	close(m);
}

/* What the opener got, from the file its script wrote and renamed to path once
 * whole; NULL when nothing came within 5 s. Polled, the opener is no child. */
static char *opener_output(const char *path)
{
	static char buf[4096];
	for (int64_t end = now_ms() + 5000; now_ms() < end;) {
		FILE *f = fopen(path, "r");
		if (f) {
			size_t n = fread(buf, 1, sizeof buf - 1, f);
			buf[n] = '\0';
			fclose(f);
			unlink(path);
			return buf;
		}
		poll(NULL, 0, 10);
	}
	return NULL;
}

/* how many fds this process has open */
static int open_fds(void)
{
	int n = 0;
	DIR *d = opendir("/proc/self/fd");
	if (!d)
		return -1;
	while (readdir(d))
		n++;
	closedir(d);
	return n;
}

static char **none_args(void)
{
	static char *a[] = { "/nonexistent/opener", "u", NULL };
	return a;
}

static void test_opener(void)
{
	char dir[] = "/tmp/test_shell.XXXXXX";
	if (!mkdtemp(dir)) {
		perror("mkdtemp");
		exit(1);
	}
	char out[256], tmp[256], script[2048];
	snprintf(out, sizeof out, "%s/out", dir);
	snprintf(tmp, sizeof tmp, "%s/tmp", dir);
	/* stdout on the file for good: a redirection of each command would keep
	 * the old one on an fd of the shell's while ls looks */
	snprintf(script, sizeof script,
		 "o=$(readlink /proc/$$/fd/1); exec >%s; echo \"O=$o\"; "
		 "echo \"T=${XDG_ACTIVATION_TOKEN-unset} U=$1 N=$#\"; "
		 "echo \"D=${DESKTOP_STARTUP_ID-unset}\"; "
		 "echo \"S=$(cut -d' ' -f6 /proc/$$/stat) P=$(cut -d' ' -f4 /proc/$$/stat)\"; "
		 "ls -m /proc/$$/fd; readlink /proc/$$/fd/0; "
		 "grep '^Sig\\(Ign\\|Blk\\)' /proc/$$/status; mv %s %s",
		 tmp, tmp, out);
	char *argv[] = { "sh", "-c", script, "opener", "http://a.b/c?d=e&f", NULL };

	/* what mwt has open, ignores and blocks is not for the opener */
	int fd = open("/dev/null", O_RDONLY);
	CHECK(dup2(fd, 3) == 3 && dup2(fd, 50) == 50);
	if (fd != 3)
		close(fd);
	signal(SIGINT, SIG_IGN);
	sigset_t block, old;
	sigemptyset(&block);
	sigaddset(&block, SIGTERM);
	sigprocmask(SIG_BLOCK, &block, &old);
	/* nor its stdin and stdout: /dev/zero, so that /dev/null there is the
	 * opener's own */
	int saved0 = dup(0), saved1 = dup(1), zero = open("/dev/zero", O_RDWR);
	dup2(zero, 0);
	dup2(zero, 1);
	close(zero);
	setenv("DESKTOP_STARTUP_ID", "stale-id", 1);
	CHECK(opener_spawn(argv, "tok-123"));
	unsetenv("DESKTOP_STARTUP_ID");
	dup2(saved0, 0);
	dup2(saved1, 1);
	close(saved0);
	close(saved1);
	sigprocmask(SIG_SETMASK, &old, NULL);
	signal(SIGINT, SIG_DFL);
	close(50);
	close(3);
	/* the child between was reaped, the opener is not one */
	errno = 0;
	CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);

	char *got = opener_output(out);
	CHECK(got);
	if (got) {
		CHECK(strstr(got, "O=/dev/null\nT=tok-123 U=http://a.b/c?d=e&f N=1\nD=tok-123\n"));
		/* a session of its own, and another parent than mwt */
		int sid = -1, ppid = -1;
		char *sp = strstr(got, "S=");
		CHECK(sp && sscanf(sp, "S=%d P=%d", &sid, &ppid) == 2);
		CHECK(sid > 0 && sid != getsid(0) && ppid != getpid());
		CHECK(strstr(got, "\n0, 1, 2\n/dev/null\n"));
		CHECK(strstr(got, "SigBlk:\t0000000000000000"));
		CHECK(strstr(got, "SigIgn:\t0000000000000000"));
	}

	/* without a token, one mwt was started with does not reach it */
	setenv("XDG_ACTIVATION_TOKEN", "stale", 1);
	setenv("DESKTOP_STARTUP_ID", "stale-id", 1);
	CHECK(opener_spawn(argv, NULL));
	got = opener_output(out);
	CHECK(got && strstr(got, "T=unset U=http://a.b/c?d=e&f N=1\nD=unset\n"));
	unsetenv("XDG_ACTIVATION_TOKEN");
	unsetenv("DESKTOP_STARTUP_ID");

	/* the mask straight, without a shell between: dash clears it itself; cp
	 * copies its own status */
	snprintf(tmp, sizeof tmp, "%s/mask", dir);
	char *mask[] = { "cp", "/proc/self/status", tmp, NULL };
	sigprocmask(SIG_BLOCK, &block, &old);
	CHECK(opener_spawn(mask, "t"));
	sigprocmask(SIG_SETMASK, &old, NULL);
	static char status[8192];
	for (int64_t end = now_ms() + 5000; now_ms() < end && !strstr(status, "SigCgt");) {
		FILE *f = fopen(tmp, "r");
		if (f) {
			size_t n = fread(status, 1, sizeof status - 1, f);
			status[n] = '\0';
			fclose(f);
		}
		poll(NULL, 0, 10);
	}
	CHECK(strstr(status, "SigBlk:\t0000000000000000\n"));
	unlink(tmp);

	/* either fork failing: false with its errno, nothing left to reap */
	for (int k = 1; k <= 2; k++) {
		forks = k;
		errno = 0;
		CHECK(!opener_spawn(none_args(), "t") && errno == ENOMEM);
		forks = 0;
		errno = 0;
		CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);
	}

	/* a program that is not there, or may not be run: false with the exec's
	 * errno, at once, and nothing left to reap or open */
	char *none[] = { "/nonexistent/opener", "u", NULL };
	char *noexec[] = { dir, "u", NULL }; /* a directory */
	int fds = open_fds();
	int64_t t0 = now_ms();
	errno = 0;
	CHECK(!opener_spawn(none, "t") && errno == ENOENT);
	errno = 0;
	CHECK(!opener_spawn(noexec, "t") && errno == EACCES);
	CHECK(now_ms() - t0 < 500);
	CHECK(open_fds() == fds);
	/* a program that is there, but the pipe could not be moved to 3 for it: it
	 * does not start, and says why; nor when the child between dies of a
	 * signal, whatever became of the opener */
	char *works[] = { "true", NULL };
	dup3_fails = true;
	errno = 0;
	CHECK(!opener_spawn(works, "t") && errno == EMFILE);
	dup3_fails = false;
	kill_middle = true;
	errno = 0;
	CHECK(!opener_spawn(works, "t") && errno == EINTR);
	kill_middle = false;
	CHECK(open_fds() == fds);
	CHECK(opener_spawn(works, "t")); /* and with neither, it does */
	errno = 0;
	CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD);

	rmdir(dir);
}

int main(void)
{
	signal(SIGPIPE, SIG_IGN); /* as mwt */
	test_pid = getpid();
	test_write();
	test_write_front();
	test_write_random();
	test_write_fail();
	test_brackets_out();
	test_reply();
	test_signal_char();
	test_read_turn();
	test_drop();
	test_gone();
	test_spawn();
	test_hangup();
	test_opener();
	printf("test_shell: %d/%d checks passed\n", checks - failures, checks);
	return failures != 0;
}
