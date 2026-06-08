#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <linux/memfd.h>

#include "memfd-cache.h"
#include "common/scm.h"
#include "log.h"

#undef LOG_PREFIX
#define LOG_PREFIX "memfd-cache: "

/* Mirror of the F_SEAL_FUTURE_WRITE bit (linux/fcntl.h, Linux 5.1+). */
#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

int memfd_cache_sock(void)
{
	static int sock = -2; /* -2: not yet probed */

	if (sock == -2) {
		char *env = getenv("CRIU_MEMFD_CACHE_SOCK");

		sock = env ? atoi(env) : -1;
		if (sock >= 0)
			pr_info("using cache socket fd %d\n", sock);
	}

	return sock;
}

bool memfd_cache_eligible(MemfdInodeEntry *mie)
{
	/*
	 * Structural pre-fork gate only: any non-hugetlb memfd inode may take part
	 * in the cache. The seals are not checked here -- the dumped weight shadows
	 * are writable (F_SEAL_SEAL, MAP_SHARED), not F_SEAL_FUTURE_WRITE.
	 *
	 * COW safety is decided later, once VMA collection has happened, in
	 * memfd_finalize_cow(): an inode mapped by a single MAP_SHARED VMA is remapped
	 * MAP_PRIVATE (copy-on-write) and its golden is sealed
	 * F_SEAL_FUTURE_WRITE before donate. The golden is then never writable-mapped,
	 * so the single shared physical copy cannot be corrupted and each borrower's
	 * writes fault to private pages. This split is required because the GET/MISS
	 * decision runs pre-fork (coordinator) while the VMA flags only become known
	 * post-fork (root task) -- see memfd_finalize_cow().
	 *
	 * hugetlb memfds are excluded from caching in v1.
	 */
	if (mie->has_hugetlb_flag && (mie->hugetlb_flag & MFD_HUGETLB))
		return false;

	return true;
}

static void build_key(struct memfd_cache_key *key, MemfdInodeEntry *mie, const char *cache_id)
{
	memset(key, 0, sizeof(*key));
	key->shmid = mie->shmid;
	key->uid = mie->uid;
	key->gid = mie->gid;
	if (cache_id)
		strncpy(key->id, cache_id, MEMFD_CACHE_ID_MAX - 1);
}

static int write_req(int sock, struct memfd_cache_req *req)
{
	ssize_t n = write(sock, req, sizeof(*req));

	if (n != (ssize_t)sizeof(*req)) {
		pr_warn("request write failed (%zd): %s\n", n, strerror(errno));
		return -1;
	}
	return 0;
}

static int read_resp(int sock, struct memfd_cache_resp *resp)
{
	ssize_t n = read(sock, resp, sizeof(*resp));

	if (n != (ssize_t)sizeof(*resp)) {
		pr_warn("response read failed (%zd): %s\n", n, strerror(errno));
		return -1;
	}
	return 0;
}

enum memfd_cache_result memfd_cache_get(int sock, MemfdInodeEntry *mie, const char *cache_id, int *out_fd)
{
	struct memfd_cache_req req = {};
	struct memfd_cache_resp resp = {};
	struct stat st;
	int fd;

	req.op = MEMFD_CACHE_OP_GET;
	req.seals = mie->seals;
	req.size = mie->size;
	build_key(&req.key, mie, cache_id);

	if (write_req(sock, &req))
		return MEMFD_CACHE_R_ERR;
	if (read_resp(sock, &resp))
		return MEMFD_CACHE_R_ERR;

	if (resp.status != MEMFD_CACHE_HIT)
		return MEMFD_CACHE_R_MISS;

	fd = recv_fd(sock);
	if (fd < 0) {
		pr_warn("HIT for shmid %#lx but recv_fd failed\n", (unsigned long)mie->shmid);
		return MEMFD_CACHE_R_ERR;
	}

	/*
	 * Belt-and-suspenders against a key bug: a HIT fd must already carry the
	 * inode's owner and size. On a userns restore the first opener's
	 * cr_fchpermat tolerates the chown EPERM only when the fd is already
	 * correctly owned, so a wrong-owner fd here would later hard-fail --
	 * reject it now and fall back to a local fill instead.
	 */
	if (fstat(fd, &st) < 0) {
		pr_warn("fstat on HIT fd failed: %s\n", strerror(errno));
		close(fd);
		return MEMFD_CACHE_R_ERR;
	}
	if (st.st_uid != mie->uid || st.st_gid != mie->gid || (uint64_t)st.st_size != mie->size) {
		pr_warn("HIT fd mismatch for shmid %#lx (uid %u/%u gid %u/%u size %llu/%llu); rejecting\n",
			(unsigned long)mie->shmid, st.st_uid, mie->uid, st.st_gid, mie->gid,
			(unsigned long long)st.st_size, (unsigned long long)mie->size);
		close(fd);
		return MEMFD_CACHE_R_MISS;
	}

	*out_fd = fd;
	return MEMFD_CACHE_R_HIT;
}

int memfd_cache_donate(int sock, int fd, MemfdInodeEntry *mie, const char *cache_id)
{
	struct memfd_cache_req req = {};
	struct memfd_cache_resp resp = {};

	req.op = MEMFD_CACHE_OP_DONATE;
	req.seals = mie->seals;
	req.size = mie->size;
	build_key(&req.key, mie, cache_id);

	if (write_req(sock, &req))
		return -1;
	if (send_fd(sock, NULL, 0, fd) < 0) {
		pr_warn("send_fd failed donating shmid %#lx\n", (unsigned long)mie->shmid);
		return -1;
	}
	if (read_resp(sock, &resp))
		return -1;

	if (resp.status != MEMFD_CACHE_OK)
		pr_debug("donation declined for shmid %#lx\n", (unsigned long)mie->shmid);
	else
		pr_debug("donated shmid %#lx (%llu bytes)\n", (unsigned long)mie->shmid,
			 (unsigned long long)mie->size);
	return 0;
}
