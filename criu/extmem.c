#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "cr_options.h"
#include "common/list.h"
#include "common/lock.h"
#include "criu-log.h"
#include "extmem.h"
#include "files.h"
#include "int.h"
#include "images/extmem.pb-c.h"
#include "rst-malloc.h"
#include "xmalloc.h"

#define EXTMEM_MAX_PACKET (1U << 20)

static bool initialized;
static bool unavailable;
static mutex_t *exchange_lock;

struct extmem_timing {
	uint64_t count;
	uint64_t total_ns;
	uint64_t max_ns;
	uint64_t bytes;
	uint64_t success;
	uint64_t unsupported;
	uint64_t error;
	uint64_t pack_ns;
	uint64_t lock_wait_ns;
	uint64_t socket_ns;
	uint64_t send_ns;
	uint64_t recv_ns;
	uint64_t decode_ns;
	uint64_t close_ns;
};

struct extmem_profile {
	struct extmem_timing timings[7];
	uint64_t session_started_ns;
	uint64_t preflight_count;
	uint64_t preflight_lookup_ns;
	uint64_t preflight_close_ns;
	uint64_t validation_count;
	uint64_t validation_ns;
};

static struct extmem_profile *profile;

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static double seconds(uint64_t ns)
{
	return (double)ns / 1000000000.0;
}

static const char *op_name(unsigned int op)
{
	static const char *names[] = { "UNKNOWN", "INIT", "OPEN_IMAGE", "GET_VMA", "GET_SHARED", "COMMIT", "ABORT" };

	return op < ARRAY_SIZE(names) ? names[op] : "UNKNOWN";
}

void extmem_report_timings(void)
{
	unsigned int op;
	uint64_t now;

	if (!profile || !profile->session_started_ns)
		return;
	now = monotonic_ns();
	dprintf(STDERR_FILENO,
		"EXTMEM_SESSION wall_s=%.6f preflight_count=%" PRIu64
		" preflight_lookup_s=%.6f preflight_close_s=%.6f"
		" validation_count=%" PRIu64 " validation_s=%.6f\n",
		seconds(now - profile->session_started_ns), profile->preflight_count,
		seconds(profile->preflight_lookup_ns), seconds(profile->preflight_close_ns),
		profile->validation_count, seconds(profile->validation_ns));

	for (op = 1; op < ARRAY_SIZE(profile->timings); op++) {
		struct extmem_timing *t = &profile->timings[op];

		if (!t->count)
			continue;
		dprintf(STDERR_FILENO,
			"EXTMEM_TIMING op=%s count=%" PRIu64 " total_s=%.6f avg_s=%.6f"
			" max_s=%.6f bytes=%" PRIu64 " success=%" PRIu64
			" unsupported=%" PRIu64 " error=%" PRIu64
			" pack_s=%.6f lock_wait_s=%.6f fdstore_get_s=%.6f"
			" send_s=%.6f recv_s=%.6f decode_s=%.6f close_s=%.6f\n",
			op_name(op), t->count, seconds(t->total_ns), seconds(t->total_ns) / t->count,
			seconds(t->max_ns), t->bytes, t->success, t->unsupported, t->error,
			seconds(t->pack_ns), seconds(t->lock_wait_ns), seconds(t->socket_ns),
			seconds(t->send_ns), seconds(t->recv_ns), seconds(t->decode_ns),
			seconds(t->close_ns));
	}
	memset(profile, 0, sizeof(*profile));
}

struct shared_fd {
	struct list_head list;
	unsigned long shmid;
	int fd;
};

static LIST_HEAD(shared_fds);

static int provider_socket(void)
{
	return inherit_fd_lookup_id("0-extmem-provider");
}

static void clear_shared_fds(void)
{
	struct shared_fd *entry, *next;

	list_for_each_entry_safe(entry, next, &shared_fds, list) {
		close(entry->fd);
		list_del(&entry->list);
		xfree(entry);
	}
}

bool extmem_enabled(void)
{
	int fd;
	uint64_t started;

	if (unavailable)
		return false;
	started = monotonic_ns();
	fd = provider_socket();
	if (profile) {
		__atomic_fetch_add(&profile->preflight_lookup_ns, monotonic_ns() - started, __ATOMIC_RELAXED);
		__atomic_fetch_add(&profile->preflight_count, 1, __ATOMIC_RELAXED);
	}
	if (fd < 0)
		return false;
	started = monotonic_ns();
	close(fd);
	if (profile)
		__atomic_fetch_add(&profile->preflight_close_ns, monotonic_ns() - started, __ATOMIC_RELAXED);
	return true;
}

static int exchange(ExtmemReq *req, int *fds, unsigned int want_fds, uint64_t *length)
{
	uint8_t packet[EXTMEM_MAX_PACKET];
	char control[CMSG_SPACE(sizeof(int) * 253)];
	struct msghdr msg = {};
	struct iovec iov;
	struct cmsghdr *cmsg;
	ExtmemResp *resp = NULL;
	int got_fds[253], nr_fds = 0;
	ssize_t len;
	int socket_fd = -1;
	size_t packed;
	int ret = -1;
	struct extmem_timing sample = {};
	uint64_t started = monotonic_ns();
	uint64_t phase_started;
	uint64_t decode_started = 0;
	uint64_t requested_bytes = 0;

	if (req->op == EXTMEM_OP__EXTMEM_GET_VMA && req->get_vma)
		requested_bytes = req->get_vma->length;
	else if (req->op == EXTMEM_OP__EXTMEM_GET_SHARED && req->get_shared)
		requested_bytes = req->get_shared->length;

	packed = extmem_req__get_packed_size(req);
	if (packed > sizeof(packet)) {
		errno = EMSGSIZE;
		return -1;
	}
	if (extmem_req__pack(req, packet) != packed)
		return -1;
	sample.pack_ns = monotonic_ns() - started;

	phase_started = monotonic_ns();
	mutex_lock(exchange_lock);
	sample.lock_wait_ns = monotonic_ns() - phase_started;
	if (!profile->session_started_ns)
		profile->session_started_ns = started;
	phase_started = monotonic_ns();
	socket_fd = provider_socket();
	sample.socket_ns = monotonic_ns() - phase_started;
	if (socket_fd < 0)
		goto out;

	iov.iov_base = packet;
	iov.iov_len = packed;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	phase_started = monotonic_ns();
	if (sendmsg(socket_fd, &msg, MSG_NOSIGNAL) != (ssize_t)packed) {
		sample.send_ns = monotonic_ns() - phase_started;
		pr_err("extmem send failed for op %d: %s\n", req->op, strerror(errno));
		goto out;
	}
	sample.send_ns = monotonic_ns() - phase_started;

	memset(&msg, 0, sizeof(msg));
	memset(control, 0, sizeof(control));
	iov.iov_base = packet;
	iov.iov_len = sizeof(packet);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	phase_started = monotonic_ns();
	len = recvmsg(socket_fd, &msg, 0);
	sample.recv_ns = monotonic_ns() - phase_started;
	if (len <= 0 || (msg.msg_flags & MSG_TRUNC) || len > (ssize_t)sizeof(packet)) {
		pr_err("extmem receive failed for op %d: %s\n", req->op, strerror(errno));
		errno = EBADMSG;
		goto out;
	}
	decode_started = monotonic_ns();

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		unsigned int n;
		if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
		    cmsg->cmsg_len < CMSG_LEN(0)) {
			errno = EBADMSG;
			goto close_fds;
		}
		n = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		if (n > ARRAY_SIZE(got_fds) - (unsigned int)nr_fds) {
			errno = EBADMSG;
			goto close_fds;
		}
		memcpy(got_fds + nr_fds, CMSG_DATA(cmsg), n * sizeof(int));
		nr_fds += n;
	}

	resp = extmem_resp__unpack(NULL, len, packet);
	if (!resp) {
		pr_err("extmem invalid response for op %d\n", req->op);
		errno = EBADMSG;
		goto close_fds;
	}
	if (resp->status) {
		pr_info("extmem provider op %d status %d\n", req->op, resp->status);
		errno = resp->status < 0 ? -resp->status : EIO;
		ret = resp->status == -ENOTSUP ? -ENOTSUP : -1;
		goto free_resp;
	}
	if (nr_fds != (int)want_fds) {
		errno = EBADMSG;
		goto free_resp;
	}
	if (length)
		*length = resp->has_length ? resp->length : 0;
	if (want_fds)
		*fds = got_fds[0];
	ret = 0;

free_resp:
	if (decode_started) {
		sample.decode_ns = monotonic_ns() - decode_started;
		decode_started = 0;
	}
	extmem_resp__free_unpacked(resp, NULL);
	resp = NULL;
	if (ret == 0)
		goto out;
close_fds:
	if (decode_started) {
		sample.decode_ns = monotonic_ns() - decode_started;
		decode_started = 0;
	}
	for (int i = 0; i < nr_fds; i++)
		close(got_fds[i]);
out:
	if (decode_started)
		sample.decode_ns = monotonic_ns() - decode_started;
	if (resp)
		extmem_resp__free_unpacked(resp, NULL);
	phase_started = monotonic_ns();
	if (socket_fd >= 0)
		close(socket_fd);
	sample.close_ns = monotonic_ns() - phase_started;
	sample.total_ns = monotonic_ns() - started;
	if ((unsigned int)req->op < ARRAY_SIZE(profile->timings)) {
		struct extmem_timing *timing = &profile->timings[req->op];

		timing->count++;
		timing->total_ns += sample.total_ns;
		if (sample.total_ns > timing->max_ns)
			timing->max_ns = sample.total_ns;
		timing->bytes += requested_bytes;
		if (!ret)
			timing->success++;
		else if (ret == -ENOTSUP)
			timing->unsupported++;
		else
			timing->error++;
		timing->pack_ns += sample.pack_ns;
		timing->lock_wait_ns += sample.lock_wait_ns;
		timing->socket_ns += sample.socket_ns;
		timing->send_ns += sample.send_ns;
		timing->recv_ns += sample.recv_ns;
		timing->decode_ns += sample.decode_ns;
		timing->close_ns += sample.close_ns;
	}
	mutex_unlock(exchange_lock);
	return ret;
}

static int request(ExtmemReq *req, int *fd, uint64_t *length)
{
	return exchange(req, fd, fd ? 1 : 0, length);
}

int extmem_init(void)
{
	ExtmemReq req = EXTMEM_REQ__INIT;
	int ret;
	if (initialized)
		return 0;
	if (!extmem_enabled())
		return -ENOTSUP;
	if (!exchange_lock) {
		exchange_lock = shmalloc(sizeof(*exchange_lock));
		if (!exchange_lock)
			return -1;
		profile = shmalloc(sizeof(*profile));
		if (!profile)
			return -1;
		memset(profile, 0, sizeof(*profile));
		mutex_init(exchange_lock);
	}
	req.op = EXTMEM_OP__EXTMEM_INIT;
	pr_info("External memory provider init\n");
	ret = request(&req, NULL, NULL);
	if (ret == -ENOTSUP)
		unavailable = true;
	if (ret)
		return ret;
	initialized = true;
	return 0;
}

int extmem_open_image(const char *name, int flags, int *fd)
{
	ExtmemOpenImage image = EXTMEM_OPEN_IMAGE__INIT;
	ExtmemReq req = EXTMEM_REQ__INIT;
	if (extmem_init())
		return extmem_enabled() ? -1 : -ENOTSUP;
	image.name = (char *)name;
	image.flags = flags;
	req.op = EXTMEM_OP__EXTMEM_OPEN_IMAGE;
	req.open_image = &image;
	return request(&req, fd, NULL);
}

int extmem_get_vma(pid_t pid, unsigned long vaddr, unsigned long length, int *fd)
{
	ExtmemGetVma vma = EXTMEM_GET_VMA__INIT;
	ExtmemReq req = EXTMEM_REQ__INIT;
	if (extmem_init())
		return extmem_enabled() ? -1 : -ENOTSUP;
	vma.pid = pid;
	vma.vaddr = vaddr;
	vma.length = length;
	req.op = EXTMEM_OP__EXTMEM_GET_VMA;
	req.get_vma = &vma;
	return request(&req, fd, NULL);
}

int extmem_get_shared(unsigned long shmid, unsigned long length, int *fd)
{
	ExtmemGetShared shared = EXTMEM_GET_SHARED__INIT;
	ExtmemReq req = EXTMEM_REQ__INIT;
	struct shared_fd *entry;
	int newfd;

	list_for_each_entry(entry, &shared_fds, list) {
		if (entry->shmid == shmid) {
			*fd = dup(entry->fd);
			return *fd < 0 ? -1 : 0;
		}
	}
	if (extmem_init())
		return extmem_enabled() ? -1 : -ENOTSUP;
	shared.shmid = shmid;
	shared.length = length;
	req.op = EXTMEM_OP__EXTMEM_GET_SHARED;
	req.get_shared = &shared;
	if (request(&req, &newfd, NULL))
		return extmem_enabled() ? -1 : -ENOTSUP;
	entry = xmalloc(sizeof(*entry));
	if (!entry) {
		close(newfd);
		return -1;
	}
	entry->shmid = shmid;
	entry->fd = newfd;
	list_add_tail(&entry->list, &shared_fds);
	*fd = dup(newfd);
	return *fd < 0 ? -1 : 0;
}

int extmem_validate_memfd(int fd, unsigned long length)
{
	struct stat st;
	int seals;
	int ret;
	uint64_t started = monotonic_ns();

	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < (off_t)length)
		ret = -1;
	else {
		seals = fcntl(fd, F_GET_SEALS);
		ret = seals < 0 ? -1 : 0;
	}
	if (profile) {
		__atomic_fetch_add(&profile->validation_count, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&profile->validation_ns, monotonic_ns() - started, __ATOMIC_RELAXED);
	}
	return ret;
}

static int end_session(ExtmemOp op)
{
	ExtmemReq req = EXTMEM_REQ__INIT;
	int ret;
	if (!initialized)
		return 0;
	req.op = op;
	ret = exchange(&req, NULL, 0, NULL);
	extmem_report_timings();
	initialized = false;
	clear_shared_fds();
	return ret;
}

int extmem_commit(void) { return end_session(EXTMEM_OP__EXTMEM_COMMIT); }
int extmem_abort(void) { return end_session(EXTMEM_OP__EXTMEM_ABORT); }
