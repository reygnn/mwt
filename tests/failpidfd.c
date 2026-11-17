/* tests/failpidfd.c - LD_PRELOAD for evil.sh no_pidfd: pidfd_open fails, as
 * before Linux 5.3 or under a seccomp filter, so that mwt goes without one.
 * mwt calls it through syscall(), which is caught here and passed on for every
 * other number. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/syscall.h>

long syscall(long n, ...)
{
	static long (*real)(long, ...);
	va_list ap;
	va_start(ap, n);
	long a[6];
	for (int i = 0; i < 6; i++)
		a[i] = va_arg(ap, long);
	va_end(ap);
	if (n == SYS_pidfd_open) {
		errno = ENOSYS;
		return -1;
	}
	if (!real)
		real = (long (*)(long, ...))dlsym(RTLD_NEXT, "syscall");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return real(n, a[0], a[1], a[2], a[3], a[4], a[5]);
}
