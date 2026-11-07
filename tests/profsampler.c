/* profsampler.c - a CPU sampler for mwt, loaded with LD_PRELOAD by `make profile`.
 *
 * perf needs perf_event_paranoid lowered, which is a setting of the host. This takes
 * ITIMER_PROF signals instead: at each one it walks the frame pointers and keeps the
 * stack, and at exit it writes them for tests/profreport.py. It costs little enough
 * to leave the timing alone, and sees kernel time only as the syscall that spent it.
 *
 * It also counts the frame callbacks mwt asks for, which is the frames it drew.
 * Only arms in a binary called mwt, and takes itself out of the child's environment. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define BYTES (64u << 20)
#define DEPTH 48

static uintptr_t *buf;
static size_t len, cap, samples;
static uintptr_t text_lo, text_hi, stack_top;
static long frames;
static struct timespec start;
static int armed;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
	(void)sig;
	(void)si;
	int saved = errno;
	ucontext_t *uc = uc_;
	uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
	uintptr_t sp = uc->uc_mcontext.gregs[REG_RSP];
	uintptr_t bp = uc->uc_mcontext.gregs[REG_RBP];
	if (len + DEPTH + 2 < cap && sp < stack_top) {
		size_t head = len++, n = 0;
		buf[len + n++] = pc;
		/* An assembler leaf in libc, like memcpy, sets up no frame: the walk would
		 * miss its caller, but the return address is still near the stack's top. */
		if (pc < text_lo || pc >= text_hi)
			for (int i = 0; i < 8 && sp + i * 8 < stack_top; i++) {
				uintptr_t v = ((uintptr_t *)sp)[i];
				if (v >= text_lo && v < text_hi) {
					buf[len + n++] = v;
					break;
				}
			}
		while (n < DEPTH && bp >= sp && bp + 16 <= stack_top && !(bp & 7)) {
			uintptr_t next = ((uintptr_t *)bp)[0];
			buf[len + n++] = ((uintptr_t *)bp)[1];
			if (next <= bp)
				break;
			bp = next;
		}
		buf[head] = n;
		len += n;
		samples++;
	}
	errno = saved;
}

int wl_proxy_add_listener(void *proxy, void (**impl)(void), void *data)
{
	static int (*real)(void *, void (**)(void), void *);
	static const char *(*class)(void *);
	if (!real) {
		real = dlsym(RTLD_NEXT, "wl_proxy_add_listener");
		class = dlsym(RTLD_NEXT, "wl_proxy_get_class");
	}
	if (!strcmp(class(proxy), "wl_callback"))
		frames++;
	return real(proxy, impl, data);
}

__attribute__((constructor)) static void arm(void)
{
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
	if (n <= 0 || !getenv("PROF_OUT"))
		return;
	exe[n] = 0;
	const char *base = strrchr(exe, '/');
	if (strcmp(base ? base + 1 : exe, "mwt"))
		return;
	unsetenv("LD_PRELOAD");
	FILE *maps = fopen("/proc/self/maps", "r");
	if (!maps)
		return;
	char line[4400];
	while (fgets(line, sizeof line, maps)) {
		unsigned long lo, hi;
		char perm[8];
		if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) != 3)
			continue;
		if (strstr(line, "[stack]"))
			stack_top = hi;
		if (!strcmp(perm, "r-xp") && strstr(line, exe)) {
			text_lo = lo;
			text_hi = hi;
		}
	}
	fclose(maps);
	cap = BYTES / sizeof *buf;
	buf = mmap(NULL, BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (buf == MAP_FAILED)
		return;
	struct sigaction sa = { 0 };
	sa.sa_sigaction = on_prof;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPROF, &sa, NULL);
	clock_gettime(CLOCK_MONOTONIC, &start);
	struct itimerval it = { { 0, 1000 }, { 0, 1000 } };
	setitimer(ITIMER_PROF, &it, NULL);
	armed = 1;
}

__attribute__((destructor)) static void disarm(void)
{
	if (!armed)
		return;
	struct itimerval off = { 0 };
	setitimer(ITIMER_PROF, &off, NULL);
	struct timespec end;
	clock_gettime(CLOCK_MONOTONIC, &end);
	struct rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	const char *out = getenv("PROF_OUT");
	char path[4096];
	snprintf(path, sizeof path, "%s.raw", out);
	FILE *f = fopen(path, "w");
	if (f) {
		fwrite(buf, sizeof *buf, len, f);
		fclose(f);
	}
	snprintf(path, sizeof path, "%s.maps", out);
	f = fopen(path, "w");
	FILE *maps = fopen("/proc/self/maps", "r");
	if (f && maps)
		for (int c; (c = fgetc(maps)) != EOF;)
			fputc(c, f);
	if (f)
		fclose(f);
	if (maps)
		fclose(maps);
	snprintf(path, sizeof path, "%s.stat", out);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%.3f %.3f %.3f %zu %ld\n",
			(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9,
			ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6,
			ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6, samples, frames);
		fclose(f);
	}
}
