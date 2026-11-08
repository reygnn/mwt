/* tests/failmemfd.c - LD_PRELOAD for evil.sh buffer_fail: memfd_create fails
 * while the file MWT_FAIL_MEMFD names exists, as on a system out of memory or
 * descriptors, so that mwt cannot make a new buffer. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int memfd_create(const char *name, unsigned int flags)
{
	static int (*real)(const char *, unsigned int);
	const char *f = getenv("MWT_FAIL_MEMFD");
	if (f && access(f, F_OK) == 0) {
		errno = ENOMEM;
		return -1;
	}
	if (!real)
		real = (int (*)(const char *, unsigned int))dlsym(RTLD_NEXT, "memfd_create");
	if (!real) {
		errno = ENOSYS;
		return -1;
	}
	return real(name, flags);
}
