#include <linux/memfd.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "memfd content restore with many parked threads";
const char *test_author = "Dan Feigin <dfeigin@nvidia.com>";

#define NR_THREADS 64
#define LEN	   PAGE_SIZE

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int done;

static int _memfd_create(const char *name, unsigned int flags)
{
	return syscall(SYS_memfd_create, name, flags);
}

static void *parked_thread(void *arg)
{
	pthread_mutex_lock(&mutex);
	while (!done)
		pthread_cond_wait(&cond, &mutex);
	pthread_mutex_unlock(&mutex);

	return NULL;
}

int main(int argc, char *argv[])
{
	pthread_t threads[NR_THREADS];
	uint8_t buf[LEN];
	uint32_t crc;
	void *map;
	int fd;
	int i;

	test_init(argc, argv);

	fd = _memfd_create("memfd-threads", MFD_CLOEXEC);
	if (fd < 0) {
		pr_perror("Can't call memfd_create");
		exit(1);
	}

	crc = ~0;
	datagen(buf, LEN, &crc);
	if (write(fd, buf, LEN) != LEN) {
		pr_perror("write error");
		exit(1);
	}

	map = mmap(NULL, LEN, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		pr_perror("Can't mmap memfd");
		exit(1);
	}

	for (i = 0; i < NR_THREADS; i++) {
		if (pthread_create(&threads[i], NULL, parked_thread, NULL)) {
			pr_perror("Can't pthread_create");
			exit(1);
		}
	}

	test_daemon();
	test_waitsig();

	/* memfd content must survive restore, via both the mapping and the fd. */
	crc = ~0;
	if (datachk(map, LEN, &crc)) {
		fail("shared mapping content mismatch");
		return 1;
	}

	if (pread(fd, buf, LEN, 0) != LEN) {
		fail("memfd read failed");
		return 1;
	}
	crc = ~0;
	if (datachk(buf, LEN, &crc)) {
		fail("memfd fd content mismatch");
		return 1;
	}

	/* Release the parked threads and reap them. */
	pthread_mutex_lock(&mutex);
	done = 1;
	pthread_cond_broadcast(&cond);
	pthread_mutex_unlock(&mutex);
	for (i = 0; i < NR_THREADS; i++)
		pthread_join(threads[i], NULL);

	pass();

	return 0;
}
