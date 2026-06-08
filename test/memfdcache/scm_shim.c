/*
 * Standalone build of CRIU's SCM_RIGHTS helpers (send_fds / __recv_fds) for the
 * memfd-cache cross-language test (test/memfdcache/cachecli.c).
 *
 * The shipping client criu/memfd-cache.c uses send_fd()/recv_fd(), which are
 * thin inlines in include/common/scm.h over send_fds()/__recv_fds() defined in
 * include/common/scm-code.c. Rather than link the whole criu object cascade,
 * we pull in that one implementation file with the exact macro recipe
 * compel/src/lib/infect.c uses (the __sys passthrough + the few kernel-style
 * helpers scm-code.c expects). This links the SAME wire code criu ships, so the
 * test cannot drift from production framing.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <stdlib.h>

/* infect.c recipe: route the syscall wrappers straight to libc. */
#define __sys(foo)     foo
#define __sys_err(ret) (-errno)

/* scm-code.c expects these (normally from common/compiler.h + common/bug.h,
 * which drag in pr_err and the rest of criu). Define the minimal equivalents. */
#ifndef min
#define min(x, y) ((x) < (y) ? (x) : (y))
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef BUG_ON
#define BUG_ON(cond)            \
	do {                    \
		if (cond)       \
			abort(); \
	} while (0)
#endif
#ifndef BUILD_BUG_ON
#define BUILD_BUG_ON(cond) ((void)sizeof(char[1 - 2 * !!(cond)]))
#endif

#include "common/scm.h"
#include "common/scm-code.c"
