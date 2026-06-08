/*
 * fixture -- a minimal process that holds a populated, F_SEAL_FUTURE_WRITE-sealed
 * memfd mapped MAP_SHARED, then waits. integration.sh dumps it with criu to get a
 * checkpoint image whose memfd inode is cache-eligible.
 *
 * Why this exists: the cache's sharing gate is F_SEAL_FUTURE_WRITE (criu/memfd.c
 * memfd_cache_eligible). No existing zdtm test produces such an inode -- memfd06
 * does not seal at all, memfd03 seals F_SEAL_WRITE (0x0008), not F_SEAL_FUTURE_WRITE
 * (0x0010) -- so none of them would ever take the cache HIT/donate path. This is
 * the faithful stand-in for a vLLM sleep() weight-shadow memfd.
 *
 * It daemonizes (fork + setsid) so it has no controlling tty and criu can dump it
 * without --shell-job, writes its post-setsid PID to argv[1] (a pidfile), then
 * pauses until SIGTERM.
 *
 *   usage: fixture <pidfile> [size_bytes]   (size default 2 MiB)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (1024 + 9)
#endif

static volatile sig_atomic_t stop;
static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	const char *pidfile;
	long size = 2 * 1024 * 1024;
	int fd;
	char *p;
	pid_t pid;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <pidfile> [size_bytes]\n", argv[0]);
		return 2;
	}
	pidfile = argv[1];
	if (argc >= 3)
		size = strtol(argv[2], NULL, 0);

	/* Daemonize: detach from the shell's session/tty so criu does not need
	 * --shell-job to dump us. */
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return 1;
	}
	if (pid > 0)
		return 0; /* parent returns; the child below is the dump target */
	if (setsid() < 0) {
		perror("setsid");
		return 1;
	}

	fd = syscall(SYS_memfd_create, "fixture", MFD_ALLOW_SEALING);
	if (fd < 0) {
		perror("memfd_create");
		return 1;
	}
	if (ftruncate(fd, size) < 0) {
		perror("ftruncate");
		return 1;
	}

	/* Populate via a writable shared mapping BEFORE sealing (F_SEAL_FUTURE_WRITE
	 * forbids future writable mappings; existing content stays). Keep the
	 * mapping so the inode is a live MAP_SHARED region at dump time. */
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memset(p, 0xA5, size);

	if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) < 0) {
		perror("F_ADD_SEALS F_SEAL_FUTURE_WRITE");
		return 1;
	}

	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	/* Publish our PID only once the sealed inode is fully set up. */
	{
		FILE *f = fopen(pidfile, "w");

		if (!f) {
			perror("fopen pidfile");
			return 1;
		}
		fprintf(f, "%d\n", (int)getpid());
		fclose(f);
	}

	while (!stop)
		pause();

	munmap(p, size);
	close(fd);
	return 0;
}
