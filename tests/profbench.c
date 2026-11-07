/* profbench.c - the layers below the window, for `make profile`: what the pty passes
 * on its own, the same with the VT core reading it, and the VT core fed from memory.
 * usage: profbench mem|pty|pty+vt file */
#define _GNU_SOURCE
#include "vt.h"

#include <fcntl.h>
#include <locale.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void no_reply(void *ud, const char *buf, size_t len)
{
	(void)ud;
	(void)buf;
	(void)len;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	const char *mode = argv[1], *file = argv[2];
	setlocale(LC_ALL, "");
	static const struct vt_callbacks cb = { .write = no_reply };
	struct vt *vt = vt_new(200, 55, 5000, &cb);
	size_t total = 0;
	double t0;
	if (!strcmp(mode, "mem")) {
		int fd = open(file, O_RDONLY);
		off_t size = lseek(fd, 0, SEEK_END);
		char *raw = malloc(size), *data = malloc(2 * size);
		if (fd < 0 || !raw || !data || pread(fd, raw, size, 0) != size)
			return 1;
		for (off_t i = 0; i < size; i++) { /* as the tty's ONLCR would */
			if (raw[i] == '\n')
				data[total++] = '\r';
			data[total++] = raw[i];
		}
		t0 = now();
		for (size_t off = 0; off < total; off += 65536)
			vt_write(vt, data + off, total - off < 65536 ? total - off : 65536);
	} else {
		static char buf[65536];
		struct winsize ws = { .ws_row = 55, .ws_col = 200 };
		int master;
		t0 = now();
		pid_t pid = forkpty(&master, NULL, NULL, &ws);
		if (pid == 0) {
			execlp("cat", "cat", file, (char *)NULL);
			_exit(127);
		}
		for (;;) {
			ssize_t n = read(master, buf, sizeof buf);
			if (n <= 0)
				break;
			total += n;
			if (!strcmp(mode, "pty+vt"))
				vt_write(vt, buf, n);
		}
		waitpid(pid, NULL, 0);
	}
	printf("%.1f", total / 1e6 / (now() - t0));
	vt_free(vt);
	return 0;
}
