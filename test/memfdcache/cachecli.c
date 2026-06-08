/*
 * cachecli -- a tiny, self-asserting driver for the SHIPPING memfd-cache client
 * (criu/memfd-cache.c), built and linked standalone for the Tier-1 cross-language
 * test. It speaks the real wire protocol to the real Go cache Server
 * (internal/memfdcache), closing the one seam the Go-only TestSessionRoundTrip
 * cannot: a divergence between the C client's framing/validation and the server.
 *
 * It expects the cache socket fd in CRIU_MEMFD_CACHE_SOCK (the Go test passes the
 * criu end of a Server session as ExtraFiles[0] -> fd 3) and runs a fixed
 * sequence, exiting non-zero on any client-visible mismatch:
 *
 *   1. GET  shmid A            -> expect MISS   (cache empty)
 *   2. DONATE a sealed memfd   -> expect clean exchange
 *   3. GET  shmid A            -> expect HIT, fstat size == donated size
 *   4. GET  shmid B            -> expect MISS   (different key)
 *   5. DONATE an UNSEALED memfd-> clean exchange; the server must DECLINE it
 *      (verified on the Go side via Stats(): exactly one sealed entry remains).
 *
 * Build (see test/memfdcache and testdata/run_integration.sh):
 *   cc -I<criu-root> -I<criu-root>/criu/include -I<criu-root>/include \
 *      cachecli.c scm_shim.c <criu-root>/criu/memfd-cache.c -o cachecli
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

#include "memfd-cache.h"

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (1024 + 9)
#endif

/* Keep this in lockstep with the size asserted in client_integration_test.go. */
#define CACHE_TEST_SIZE 8192
#define CACHE_TEST_ID	"ckpt:1"
#define SHMID_HIT	0x1234 /* donated + borrowed back */
#define SHMID_MISS	0x9999 /* never donated */
#define SHMID_UNSEALED	0x5555 /* donated unsealed -> declined by server */

/*
 * The real memfd-cache.c logs via print_on_level (from log.h). Provide a minimal
 * sink so we link the shipping client unchanged instead of forking it for tests.
 */
void print_on_level(unsigned int loglevel, const char *format, ...)
{
	va_list ap;

	(void)loglevel;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

#define FAIL(...)                                  \
	do {                                       \
		fprintf(stderr, "cachecli: FAIL: " __VA_ARGS__); \
		fprintf(stderr, "\n");             \
		exit(1);                           \
	} while (0)

#define OK(...)                                    \
	do {                                       \
		fprintf(stderr, "cachecli: ok: " __VA_ARGS__);   \
		fprintf(stderr, "\n");             \
	} while (0)

static int memfd_create_sized(unsigned int flags, off_t size)
{
	int fd = syscall(SYS_memfd_create, "cachecli", flags);

	if (fd < 0)
		FAIL("memfd_create: %s", strerror(errno));
	if (ftruncate(fd, size) < 0)
		FAIL("ftruncate: %s", strerror(errno));
	/* Populate one marker byte before any seal so the inode is non-empty. */
	if (pwrite(fd, "X", 1, 0) != 1)
		FAIL("pwrite: %s", strerror(errno));
	return fd;
}

/* A populated memfd sealed F_SEAL_FUTURE_WRITE: exactly what CRIU donates. */
static int make_sealed_memfd(void)
{
	int fd = memfd_create_sized(MFD_ALLOW_SEALING, CACHE_TEST_SIZE);

	if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) < 0)
		FAIL("F_ADD_SEALS F_SEAL_FUTURE_WRITE: %s", strerror(errno));
	return fd;
}

/* An unsealed memfd: the server must reject this on its independent seal check. */
static int make_unsealed_memfd(void)
{
	return memfd_create_sized(MFD_ALLOW_SEALING, CACHE_TEST_SIZE);
}

/* Stack-build a MemfdInodeEntry the way the client reads it: scalar fields only,
 * base/name left zero (the client never touches the protobuf-c machinery). */
static void init_mie(MemfdInodeEntry *mie, uint32_t shmid, uint32_t seals)
{
	memset(mie, 0, sizeof(*mie));
	mie->shmid = shmid;
	mie->uid = getuid();
	mie->gid = getgid();
	mie->size = CACHE_TEST_SIZE;
	mie->seals = seals;
}

int main(void)
{
	int sock = memfd_cache_sock();
	MemfdInodeEntry mie;
	int fd = -1;
	enum memfd_cache_result r;

	if (sock < 0)
		FAIL("CRIU_MEMFD_CACHE_SOCK not set / invalid (got fd %d)", sock);

	/* 1. GET before any donate -> MISS. */
	init_mie(&mie, SHMID_HIT, F_SEAL_FUTURE_WRITE);
	r = memfd_cache_get(sock, &mie, CACHE_TEST_ID, &fd);
	if (r != MEMFD_CACHE_R_MISS)
		FAIL("pre-donate GET = %d, want MISS(%d)", r, MEMFD_CACHE_R_MISS);
	OK("pre-donate GET MISS");

	/* 2. DONATE a sealed memfd. */
	{
		int sealed = make_sealed_memfd();

		if (!memfd_cache_eligible(&mie))
			FAIL("sealed inode reported ineligible by memfd_cache_eligible");
		if (memfd_cache_donate(sock, sealed, &mie, CACHE_TEST_ID) != 0)
			FAIL("DONATE sealed: transport error");
		close(sealed);
		OK("DONATE sealed (shmid %#x)", SHMID_HIT);
	}

	/* 3. GET again -> HIT, with a correctly-sized fd. */
	fd = -1;
	r = memfd_cache_get(sock, &mie, CACHE_TEST_ID, &fd);
	if (r != MEMFD_CACHE_R_HIT)
		FAIL("post-donate GET = %d, want HIT(%d)", r, MEMFD_CACHE_R_HIT);
	{
		struct stat st;

		if (fstat(fd, &st) < 0)
			FAIL("fstat HIT fd: %s", strerror(errno));
		if ((uint64_t)st.st_size != (uint64_t)CACHE_TEST_SIZE)
			FAIL("HIT fd size = %lld, want %d", (long long)st.st_size, CACHE_TEST_SIZE);
		close(fd);
		OK("post-donate GET HIT (size %d)", CACHE_TEST_SIZE);
	}

	/* 4. GET a different key -> MISS. */
	init_mie(&mie, SHMID_MISS, F_SEAL_FUTURE_WRITE);
	fd = -1;
	r = memfd_cache_get(sock, &mie, CACHE_TEST_ID, &fd);
	if (r != MEMFD_CACHE_R_MISS)
		FAIL("other-shmid GET = %d, want MISS(%d)", r, MEMFD_CACHE_R_MISS);
	OK("other-shmid GET MISS");

	/* 5. DONATE an unsealed memfd. The exchange is clean (return 0) but the
	 * server must DECLINE the store -- verified on the Go side via Stats(). */
	{
		int unsealed = make_unsealed_memfd();

		init_mie(&mie, SHMID_UNSEALED, 0);
		if (memfd_cache_eligible(&mie))
			FAIL("unsealed inode reported eligible by memfd_cache_eligible");
		if (memfd_cache_donate(sock, unsealed, &mie, CACHE_TEST_ID) != 0)
			FAIL("DONATE unsealed: transport error");
		close(unsealed);
		OK("DONATE unsealed (server-side decline checked by Go Stats)");
	}

	OK("all client-visible assertions passed");
	return 0;
}
