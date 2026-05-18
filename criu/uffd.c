#include <stddef.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <limits.h>
#include <linux/futex.h>

#include "linux/userfaultfd.h"

#include "int.h"
#include "page.h"
#include "criu-log.h"
#include "criu-plugin.h"
#include "pagemap.h"
#include "files-reg.h"
#include "kerndat.h"
#include "mem.h"
#include "uffd.h"
#include "util-pie.h"
#include "protobuf.h"
#include "pstree.h"
#include "crtools.h"
#include "cr_options.h"
#include "xmalloc.h"
#include <compel/plugins/std/syscall-codes.h>
#include "restorer.h"
#include "page-xfer.h"
#include "common/lock.h"
#include "rst-malloc.h"
#include "tls.h"
#include "fdstore.h"
#include "util.h"
#include "namespaces.h"
#include "common/scm.h"

#undef LOG_PREFIX
#define LOG_PREFIX "uffd: "

#define lp_debug(lpi, fmt, arg...)  pr_debug("%d-%d: " fmt, lpi->pid, lpi->lpfd.fd, ##arg)
#define lp_info(lpi, fmt, arg...)   pr_info("%d-%d: " fmt, lpi->pid, lpi->lpfd.fd, ##arg)
#define lp_warn(lpi, fmt, arg...)   pr_warn("%d-%d: " fmt, lpi->pid, lpi->lpfd.fd, ##arg)
#define lp_err(lpi, fmt, arg...)    pr_err("%d-%d: " fmt, lpi->pid, lpi->lpfd.fd, ##arg)
#define lp_perror(lpi, fmt, arg...) pr_perror("%d-%d: " fmt, lpi->pid, lpi->lpfd.fd, ##arg)

#define NEED_UFFD_API_FEATURES \
	(UFFD_FEATURE_EVENT_FORK | UFFD_FEATURE_EVENT_REMAP | UFFD_FEATURE_EVENT_UNMAP | UFFD_FEATURE_EVENT_REMOVE)

#define LAZY_PAGES_SOCK_NAME "lazy-pages.socket"

#define LAZY_PAGES_RESTORE_FINISHED 0x52535446 /* ReSTore Finished */

/*
 * Background transfer parameters.
 * The default xfer length is arbitrary set to 64Kbytes
 * The limit of 4Mbytes matches the maximal chunk size we can have in
 * a pipe in the page-server
 */
#define DEFAULT_XFER_LEN (64 << 10)
#define MAX_XFER_LEN	 (4 << 20)

static mutex_t *lazy_sock_mutex;

struct lazy_iov {
	struct list_head l;
	unsigned long start;	 /* run-time start address, tracks remaps */
	unsigned long end;	 /* run-time end address, tracks remaps */
	unsigned long img_start; /* start address at the dump time */
};

struct lazy_pages_info {
	int pid;
	bool exited;

	struct list_head iovs;
	struct list_head reqs;

	struct lazy_pages_info *parent;
	unsigned ref_cnt;

	struct page_read pr;

	unsigned long xfer_len; /* in pages */
	unsigned long total_pages;
	unsigned long copied_pages;

	struct epoll_rfd lpfd;

	struct list_head l;

	unsigned long buf_size;
	void *buf;
};

/* global lazy-pages daemon state */
static LIST_HEAD(lpis);
static LIST_HEAD(exiting_lpis);
static LIST_HEAD(pending_lpis);
static int epollfd;
static bool restore_finished;
static struct epoll_rfd lazy_sk_rfd;
/* socket for communication with lazy-pages daemon */
static int lazy_pages_sk_id = -1;

/*
 * Pipeline C (stream-restore) daemon state. The streamer process feeds
 * memfd content over a side channel and signals readiness via eventfds.
 * - streamer_abort_rfd: eventfd owned by the streamer; POLLIN means
 *   graceful abort, POLLHUP means streamer died. Either fires the
 *   futex-poison path so PIE bails out of io_submit cleanly.
 * - streamer_evfd_rfds: per-memfd ready eventfds (count matches the
 *   manifest's shmem entry count). UFFDIO_CONTINUE installs the bytes
 *   into the restored shmem VMA range once the corresponding eventfd
 *   fires.
 * - streamer_ready_futex: shmalloc'd futex array shared with PIE; one
 *   word per vma_ios entry. Values: 0 = not ready, 1 = ready,
 *   0xFFFFFFFFu = abort.
 * All scaffolding is no-op until commit 7 wires producers/consumers.
 */
static struct epoll_rfd streamer_abort_rfd = { .fd = -1 };
static struct epoll_rfd *streamer_evfd_rfds;
static unsigned int streamer_evfd_n;
static unsigned int *streamer_ready_futex;
static unsigned int streamer_ready_futex_n;

/*
 * Stage 2c shmem zero-copy state. streamer_evfd_shmids[idx] is the shmid
 * carried by daemon handshake; streamer_evfd_vmas[idx] is the list of
 * resolved (lpi, vma_start, vma_end) tuples for that shmid across every
 * restored task. streamer_pending_faults[idx] queues faults whose range
 * has not yet been NIXL-filled — drained by handle_streamer_evfd.
 *
 * The daemon is single-threaded (epoll loop), so all access is naturally
 * serialized. streamer_ready_futex[idx] uses 0/1/0xFFFFFFFFu values; the
 * daemon never FUTEX_WAITs on it (deadlock risk), only reads/writes; PIE
 * has its own futex array for the io_submit gating.
 */
struct streamer_evfd_vma {
	struct lazy_pages_info *lpi;
	uintptr_t start;
	uintptr_t end;
	struct list_head l;
};

struct streamer_pending_fault {
	struct lazy_pages_info *lpi;
	uintptr_t addr;
	struct list_head l;
};

static uint64_t *streamer_evfd_shmids;     /* [streamer_evfd_n], host order */
static struct list_head *streamer_evfd_vmas;       /* [streamer_evfd_n] list heads */
static struct list_head *streamer_pending_faults;  /* [streamer_evfd_n] list heads */

static int handle_uffd_event(struct epoll_rfd *lpfd);
static int handle_streamer_evfd(struct epoll_rfd *rfd);
static int handle_abort_event(struct epoll_rfd *rfd);
static int handle_abort_hangup(struct epoll_rfd *rfd);

/* Forward declarations for the Stage 2c shmem zero-copy block. The
 * definitions sit lower in the file with the rest of the streamer helpers
 * so they have access to local helpers (init_mm_entry, uffd_zero, etc.). */
static int uffd_continue_memfd(struct lazy_pages_info *lpi,
			       uintptr_t addr, size_t len);
static int streamer_vma_lookup(struct lazy_pages_info *lpi, uintptr_t addr);
static int streamer_defer_fault(int idx, struct lazy_pages_info *lpi,
				uintptr_t addr);
static int streamer_drain_pending(int idx);
static void streamer_abort_pending(void);
static int build_streamer_evfd_vmas(void);

/*
 * Pipeline C: receive the streamer-owned eventfds and abort_fd over the
 * agent-pre-created Unix socket. Socket fd is inherited via the
 * CRIU_STREAMER_DAEMON_SOCK env var (same pattern as
 * CRIU_STREAMER_PRIVATE_SOCK in mem.c).
 *
 * Wire protocol (one connection, sequential):
 *   1) plain recv() of one uint32 n_evfd header (host byte order)
 *   2) recv_fds() of (1 + n_evfd) fds: [abort_fd, ev_0..ev_{n-1}]
 *      no payload bytes
 *
 * After the recv we shmalloc the daemon-side streamer_ready_futex[n]
 * array. PIE has its own per-task streamer_private_ready_futex[] in
 * task_restore_args (see mem.c:alloc_streamer_futex_array); the two
 * arrays serve different consumers (daemon's UFFDIO_CONTINUE side
 * vs PIE's io_submit side) and the streamer signals both.
 *
 * No-op if --stream-restore is off or the env var is unset.
 */
static int recv_streamer_daemon_fds(void)
{
	const char *env = getenv("CRIU_STREAMER_DAEMON_SOCK");
	int sock, *fds = NULL;
	uint32_t n;
	unsigned int i;

	if (!opts.stream_restore)
		return 0;

	if (!env) {
		pr_err("--stream-restore set but CRIU_STREAMER_DAEMON_SOCK is unset\n");
		return -1;
	}
	sock = atoi(env);
	if (sock < 0) {
		pr_err("CRIU_STREAMER_DAEMON_SOCK=%s is not a valid fd\n", env);
		return -1;
	}

	if (recv(sock, &n, sizeof(n), MSG_WAITALL) != (ssize_t)sizeof(n)) {
		pr_perror("[stream-c] failed to recv n_evfd header on daemon sock");
		return -1;
	}
	if (n == 0 || n > 1024) {
		pr_err("[stream-c] bogus n_evfd %u\n", n);
		return -1;
	}

	/*
	 * Stage 2c: per-evfd shmid table sent inline before the SCM_RIGHTS
	 * payload. Daemon uses this to resolve which shmem VMAs each evfd
	 * gates and then to dispatch on-fault UFFDIO_CONTINUE.
	 */
	streamer_evfd_shmids = xmalloc(sizeof(uint64_t) * n);
	if (!streamer_evfd_shmids)
		return -1;
	if (recv(sock, streamer_evfd_shmids, sizeof(uint64_t) * n, MSG_WAITALL) !=
	    (ssize_t)(sizeof(uint64_t) * n)) {
		pr_perror("[stream-c] failed to recv %u shmid(s) on daemon sock", n);
		xfree(streamer_evfd_shmids);
		streamer_evfd_shmids = NULL;
		return -1;
	}

	fds = xmalloc(sizeof(int) * (n + 1));
	if (!fds)
		return -1;

	if (recv_fds(sock, fds, n + 1, NULL, 0) < 0) {
		pr_err("[stream-c] recv_fds(%u) on daemon sock failed\n", n + 1);
		xfree(fds);
		return -1;
	}

	streamer_abort_rfd.fd = fds[0];

	streamer_evfd_rfds = xzalloc(sizeof(*streamer_evfd_rfds) * n);
	if (!streamer_evfd_rfds) {
		xfree(fds);
		return -1;
	}
	for (i = 0; i < n; i++)
		streamer_evfd_rfds[i].fd = fds[i + 1];
	streamer_evfd_n = n;
	xfree(fds);

	streamer_ready_futex = shmalloc(sizeof(unsigned int) * n);
	if (!streamer_ready_futex) {
		pr_err("[stream-c] shmalloc streamer_ready_futex[%u] failed\n", n);
		return -1;
	}
	memset(streamer_ready_futex, 0, sizeof(unsigned int) * n);
	streamer_ready_futex_n = n;

	streamer_evfd_vmas = xmalloc(sizeof(*streamer_evfd_vmas) * n);
	streamer_pending_faults = xmalloc(sizeof(*streamer_pending_faults) * n);
	if (!streamer_evfd_vmas || !streamer_pending_faults) {
		pr_err("[stream-c] alloc streamer_evfd state[%u] failed\n", n);
		return -1;
	}
	for (i = 0; i < n; i++) {
		INIT_LIST_HEAD(&streamer_evfd_vmas[i]);
		INIT_LIST_HEAD(&streamer_pending_faults[i]);
	}

	pr_info("[stream-c] daemon got abort_fd=%d, %u eventfds, futex[%u], shmids[%u]\n",
		streamer_abort_rfd.fd, streamer_evfd_n, streamer_ready_futex_n, n);
	for (i = 0; i < n; i++)
		pr_info("[stream-c]   shmid[%u]=0x%lx\n", i,
			(unsigned long)streamer_evfd_shmids[i]);
	return 0;
}

static struct lazy_pages_info *lpi_init(void)
{
	struct lazy_pages_info *lpi = NULL;

	lpi = xmalloc(sizeof(*lpi));
	if (!lpi)
		return NULL;

	memset(lpi, 0, sizeof(*lpi));
	INIT_LIST_HEAD(&lpi->iovs);
	INIT_LIST_HEAD(&lpi->reqs);
	INIT_LIST_HEAD(&lpi->l);
	lpi->lpfd.read_event = handle_uffd_event;
	lpi->xfer_len = DEFAULT_XFER_LEN;
	lpi->ref_cnt = 1;

	return lpi;
}

static void free_iovs(struct lazy_pages_info *lpi)
{
	struct lazy_iov *p, *n;

	list_for_each_entry_safe(p, n, &lpi->iovs, l) {
		list_del(&p->l);
		xfree(p);
	}

	list_for_each_entry_safe(p, n, &lpi->reqs, l) {
		list_del(&p->l);
		xfree(p);
	}
}

static void lpi_fini(struct lazy_pages_info *lpi);

static inline void lpi_put(struct lazy_pages_info *lpi)
{
	lpi->ref_cnt--;
	if (!lpi->ref_cnt)
		lpi_fini(lpi);
}

static inline void lpi_get(struct lazy_pages_info *lpi)
{
	lpi->ref_cnt++;
}

static void lpi_fini(struct lazy_pages_info *lpi)
{
	if (!lpi)
		return;
	xfree(lpi->buf);
	free_iovs(lpi);
	if (lpi->lpfd.fd > 0)
		close(lpi->lpfd.fd);
	if (lpi->parent)
		lpi_put(lpi->parent);
	if (!lpi->parent && lpi->pr.close)
		lpi->pr.close(&lpi->pr);
	xfree(lpi);
}

static int prepare_sock_addr(struct sockaddr_un *saddr)
{
	int len;

	memset(saddr, 0, sizeof(struct sockaddr_un));

	saddr->sun_family = AF_UNIX;
	len = snprintf(saddr->sun_path, sizeof(saddr->sun_path), "%s", LAZY_PAGES_SOCK_NAME);
	if (len >= sizeof(saddr->sun_path)) {
		pr_err("Wrong UNIX socket name: %s\n", LAZY_PAGES_SOCK_NAME);
		return -1;
	}

	return 0;
}

static int send_uffd(int sendfd, int pid)
{
	int fd;
	int ret = -1;

	if (sendfd < 0)
		return -1;

	fd = fdstore_get(lazy_pages_sk_id);
	if (fd < 0) {
		pr_err("%s: get_service_fd\n", __func__);
		return -1;
	}

	mutex_lock(lazy_sock_mutex);

	/* The "transfer protocol" is first the pid as int and then
	 * the FD for UFFD */
	pr_debug("Sending PID %d\n", pid);
	if (send(fd, &pid, sizeof(pid), 0) < 0) {
		pr_perror("PID sending error");
		goto out;
	}

	/* for a zombie process pid will be negative */
	if (pid < 0) {
		ret = 0;
		goto out;
	}

	if (send_fd(fd, NULL, 0, sendfd) < 0) {
		pr_err("send_fd error\n");
		goto out;
	}

	ret = 0;
out:
	mutex_unlock(lazy_sock_mutex);
	close(fd);
	return ret;
}

int lazy_pages_setup_zombie(int pid)
{
	if (!opts.lazy_pages && !opts.stream_restore)
		return 0;

	if (send_uffd(0, -pid))
		return -1;

	return 0;
}

bool uffd_noncooperative(void)
{
	unsigned long features = NEED_UFFD_API_FEATURES;

	return (kdat.uffd_features & features) == features;
}

static int uffd_api_ioctl(void *arg, int fd, pid_t pid)
{
	struct uffdio_api *uffdio_api = arg;

	return ioctl(fd, UFFDIO_API, uffdio_api);
}

int uffd_open(int flags, unsigned long *features, int *err)
{
	struct uffdio_api uffdio_api = { 0 };
	int uffd;

	uffd = syscall(SYS_userfaultfd, flags);
	if (uffd == -1) {
		pr_info("Lazy pages are not available: %s\n", strerror(errno));
		if (err)
			*err = errno;
		return -1;
	}

	uffdio_api.api = UFFD_API;
	if (features)
		uffdio_api.features = *features;

	if (userns_call(uffd_api_ioctl, 0, &uffdio_api, sizeof(uffdio_api), uffd)) {
		pr_perror("Failed to get uffd API");
		goto close;
	}

	if (uffdio_api.api != UFFD_API) {
		pr_err("Incompatible uffd API: expected %llu, got %llu\n", UFFD_API, uffdio_api.api);
		goto close;
	}

	if (features)
		*features = uffdio_api.features;

	return uffd;

close:
	close(uffd);
	return -1;
}

/* This function is used by 'criu restore --lazy-pages' */
int setup_uffd(int pid, struct task_restore_args *task_args)
{
	unsigned long features = kdat.uffd_features & NEED_UFFD_API_FEATURES;

	if (!opts.lazy_pages && !opts.stream_restore) {
		task_args->uffd = -1;
		return 0;
	}

	/*
	 * In stream-restore (Pipeline C) mode, also request
	 * UFFD_FEATURE_MINOR_SHMEM so the daemon can use UFFDIO_CONTINUE
	 * to install streamer-filled shmem pages on demand. The bit is
	 * only OR'd in if the running kernel actually advertises it
	 * (kdat.uffd_features); otherwise UFFDIO_API would reject the
	 * request.
	 */
	if (opts.stream_restore)
		features |= (kdat.uffd_features & UFFD_FEATURE_MINOR_SHMEM);

	/*
	 * Open userfaulfd FD which is passed to the restorer blob and
	 * to a second process handling the userfaultfd page faults.
	 */
	task_args->uffd = uffd_open(O_CLOEXEC | O_NONBLOCK, &features, NULL);
	if (task_args->uffd < 0) {
		pr_perror("Unable to open an userfaultfd descriptor");
		return -1;
	}

	if (send_uffd(task_args->uffd, pid) < 0)
		goto err;

	return 0;
err:
	close(task_args->uffd);
	return -1;
}

int prepare_lazy_pages_socket(void)
{
	int fd, len, ret = -1;
	struct sockaddr_un sun;

	if (!opts.lazy_pages && !opts.stream_restore)
		return 0;

	if (prepare_sock_addr(&sun))
		return -1;

	lazy_sock_mutex = shmalloc(sizeof(*lazy_sock_mutex));
	if (!lazy_sock_mutex)
		return -1;

	mutex_init(lazy_sock_mutex);

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	len = offsetof(struct sockaddr_un, sun_path) + strlen(sun.sun_path);
	if (connect(fd, (struct sockaddr *)&sun, len) < 0) {
		pr_perror("connect to %s failed", sun.sun_path);
		goto out;
	}

	lazy_pages_sk_id = fdstore_add(fd);
	if (lazy_pages_sk_id < 0) {
		pr_perror("Can't add fd to fdstore");
		goto out;
	}

	ret = 0;
out:
	close(fd);
	return ret;
}

static int server_listen(struct sockaddr_un *saddr)
{
	int fd;
	int len;

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		return -1;

	unlink(saddr->sun_path);

	len = offsetof(struct sockaddr_un, sun_path) + strlen(saddr->sun_path);

	if (bind(fd, (struct sockaddr *)saddr, len) < 0) {
		goto out;
	}

	if (listen(fd, 10) < 0) {
		goto out;
	}

	return fd;

out:
	close(fd);
	return -1;
}

static MmEntry *init_mm_entry(struct lazy_pages_info *lpi)
{
	struct cr_img *img;
	MmEntry *mm;
	int ret;

	img = open_image(CR_FD_MM, O_RSTR, lpi->pid);
	if (!img)
		return NULL;

	ret = pb_read_one_eof(img, &mm, PB_MM);
	close_image(img);
	if (ret == -1)
		return NULL;
	lp_debug(lpi, "Found %zd VMAs in image\n", mm->n_vmas);

	return mm;
}

static struct lazy_iov *find_iov(struct lazy_pages_info *lpi, unsigned long addr)
{
	struct lazy_iov *iov;

	list_for_each_entry(iov, &lpi->iovs, l)
		if (addr >= iov->start && addr < iov->end)
			return iov;

	return NULL;
}

static int split_iov(struct lazy_iov *iov, unsigned long addr)
{
	struct lazy_iov *new;

	new = xzalloc(sizeof(*new));
	if (!new)
		return -1;

	new->start = addr;
	new->img_start = iov->img_start + addr - iov->start;
	new->end = iov->end;
	iov->end = addr;
	list_add(&new->l, &iov->l);

	return 0;
}

static void iov_list_insert(struct lazy_iov *new, struct list_head *dst)
{
	struct lazy_iov *iov;

	if (list_empty(dst)) {
		list_move(&new->l, dst);
		return;
	}

	list_for_each_entry(iov, dst, l) {
		if (new->start < iov->start) {
			list_move_tail(&new->l, &iov->l);
			break;
		}
		if (list_is_last(&iov->l, dst) && new->start > iov->start) {
			list_move(&new->l, &iov->l);
			break;
		}
	}
}

static void merge_iov_lists(struct list_head *src, struct list_head *dst)
{
	struct lazy_iov *iov, *n;

	if (list_empty(src))
		return;

	list_for_each_entry_safe(iov, n, src, l)
		iov_list_insert(iov, dst);
}

static int __copy_iov_list(struct list_head *src, struct list_head *dst)
{
	struct lazy_iov *iov, *new;

	list_for_each_entry(iov, src, l) {
		new = xzalloc(sizeof(*new));
		if (!new)
			return -1;

		new->start = iov->start;
		new->img_start = iov->img_start;
		new->end = iov->end;

		list_add_tail(&new->l, dst);
	}

	return 0;
}

static int copy_iovs(struct lazy_pages_info *src, struct lazy_pages_info *dst)
{
	if (__copy_iov_list(&src->iovs, &dst->iovs))
		goto free_iovs;

	if (__copy_iov_list(&src->reqs, &dst->reqs))
		goto free_iovs;

	/*
	 * The IOVs already in flight for the parent process need to be
	 * transferred again for the child process
	 */
	merge_iov_lists(&dst->reqs, &dst->iovs);

	dst->buf_size = src->buf_size;
	if (posix_memalign(&dst->buf, PAGE_SIZE, dst->buf_size))
		goto free_iovs;

	return 0;

free_iovs:
	free_iovs(dst);
	return -1;
}

/*
 * Purge range (addr, addr + len) from lazy_iovs. The range may
 * cover several continuous IOVs.
 */
static int __drop_iovs(struct list_head *iovs, unsigned long addr, int len)
{
	struct lazy_iov *iov, *n;

	list_for_each_entry_safe(iov, n, iovs, l) {
		unsigned long start = iov->start;
		unsigned long end = iov->end;

		if (len <= 0 || addr + len < start)
			break;

		if (addr >= end)
			continue;

		if (addr < start) {
			len -= (start - addr);
			addr = start;
		}

		/*
		 * The range completely fits into the current IOV.
		 * If addr equals iov_start we just "drop" the
		 * beginning of the IOV. Otherwise, we make the IOV to
		 * end at addr, and add a new IOV start starts at
		 * addr + len.
		 */
		if (addr + len < end) {
			if (addr == start) {
				iov->start += len;
				iov->img_start += len;
			} else {
				if (split_iov(iov, addr + len))
					return -1;
				iov->end = addr;
			}
			break;
		}

		/*
		 * The range spawns beyond the end of the current IOV.
		 * If addr equals iov_start we just "drop" the entire
		 * IOV.  Otherwise, we cut the beginning of the IOV
		 * and continue to the next one with the updated range
		 */
		if (addr == start) {
			list_del(&iov->l);
			xfree(iov);
		} else {
			iov->end = addr;
		}

		len -= (end - addr);
		addr = end;
	}

	return 0;
}

static int drop_iovs(struct lazy_pages_info *lpi, unsigned long addr, int len)
{
	if (__drop_iovs(&lpi->iovs, addr, len))
		return -1;

	if (__drop_iovs(&lpi->reqs, addr, len))
		return -1;

	return 0;
}

static struct lazy_iov *extract_range(struct lazy_iov *iov, unsigned long start, unsigned long end)
{
	/* move the IOV tail into a new IOV */
	if (end < iov->end)
		if (split_iov(iov, end))
			return NULL;

	if (start == iov->start)
		return iov;

	/* after splitting the IOV head we'll need the ->next IOV */
	if (split_iov(iov, start))
		return NULL;

	return list_entry(iov->l.next, struct lazy_iov, l);
}

static int __remap_iovs(struct list_head *iovs, unsigned long from, unsigned long to, unsigned long len)
{
	LIST_HEAD(remaps);

	unsigned long off = to - from;
	struct lazy_iov *iov, *n;

	list_for_each_entry_safe(iov, n, iovs, l) {
		if (from >= iov->end)
			continue;

		if (len <= 0 || from + len <= iov->start)
			break;

		if (from < iov->start) {
			len -= (iov->start - from);
			from = iov->start;
		}

		if (from > iov->start) {
			if (split_iov(iov, from))
				return -1;
			list_safe_reset_next(iov, n, l);
			continue;
		}

		if (from + len < iov->end) {
			if (split_iov(iov, from + len))
				return -1;
			list_safe_reset_next(iov, n, l);
		}

		/* here we have iov->start = from, iov->end <= from + len */
		from = iov->end;
		len -= iov->end - iov->start;
		iov->start += off;
		iov->end += off;
		list_move_tail(&iov->l, &remaps);
	}

	merge_iov_lists(&remaps, iovs);

	return 0;
}

static int remap_iovs(struct lazy_pages_info *lpi, unsigned long from, unsigned long to, unsigned long len)
{
	if (__remap_iovs(&lpi->iovs, from, to, len))
		return -1;

	if (__remap_iovs(&lpi->reqs, from, to, len))
		return -1;

	return 0;
}

/*
 * Create a list of IOVs that can be handled using userfaultfd. The
 * IOVs generally correspond to lazy pagemap entries, except the cases
 * when a single pagemap entry covers several VMAs. In those cases
 * IOVs are split at VMA boundaries because UFFDIO_COPY may be done
 * only inside a single VMA.
 * We assume here that pagemaps and VMAs are sorted.
 */
static int collect_iovs(struct lazy_pages_info *lpi)
{
	unsigned long start, end, len, nr_pages = 0;
	int n_vma = 0, max_iov_len = 0, ret = -1;
	struct page_read *pr = &lpi->pr;
	struct lazy_iov *iov;
	MmEntry *mm;

	mm = init_mm_entry(lpi);
	if (!mm)
		return -1;

	while (pr->advance(pr)) {
		if (!pagemap_lazy(pr->pe))
			continue;

		start = pr->pe->vaddr;
		end = start + pr->pe->nr_pages * page_size();
		nr_pages += pr->pe->nr_pages;

		for (; n_vma < mm->n_vmas; n_vma++) {
			VmaEntry *vma = mm->vmas[n_vma];

			if (start >= vma->end)
				continue;

			iov = xzalloc(sizeof(*iov));
			if (!iov)
				goto free_iovs;

			len = min_t(uint64_t, end, vma->end) - start;
			iov->start = start;
			iov->img_start = start;
			iov->end = iov->start + len;
			list_add_tail(&iov->l, &lpi->iovs);

			if (len > max_iov_len)
				max_iov_len = len;

			if (end <= vma->end)
				break;

			start = vma->end;
		}
	}

	lpi->buf_size = max_iov_len;
	if (posix_memalign(&lpi->buf, PAGE_SIZE, lpi->buf_size))
		goto free_iovs;

	ret = nr_pages;
	goto free_mm;

free_iovs:
	free_iovs(lpi);
free_mm:
	mm_entry__free_unpacked(mm, NULL);

	return ret;
}

static int uffd_io_complete(struct page_read *pr, unsigned long vaddr, unsigned long nr);

static int ud_open(int client, struct lazy_pages_info **_lpi)
{
	struct lazy_pages_info *lpi;
	int ret = -1;
	int pr_flags = PR_TASK;

	lpi = lpi_init();
	if (!lpi)
		goto out;

	/* The "transfer protocol" is first the pid as int and then
	 * the FD for UFFD */
	ret = recv(client, &lpi->pid, sizeof(lpi->pid), 0);
	if (ret != sizeof(lpi->pid)) {
		if (ret < 0)
			pr_perror("PID recv error");
		else
			pr_err("PID recv: short read\n");
		goto out;
	}

	if (lpi->pid < 0) {
		pr_debug("Zombie PID: %d\n", lpi->pid);
		lpi_fini(lpi);
		return 0;
	}

	lpi->lpfd.fd = recv_fd(client);
	if (lpi->lpfd.fd < 0) {
		pr_err("recv_fd error\n");
		goto out;
	}
	pr_debug("Received PID: %d, uffd: %d\n", lpi->pid, lpi->lpfd.fd);

	if (opts.use_page_server)
		pr_flags |= PR_REMOTE;
	ret = open_page_read(lpi->pid, &lpi->pr, pr_flags);
	if (ret <= 0) {
		lp_err(lpi, "Failed to open pagemap\n");
		goto out;
	}

	lpi->pr.io_complete = uffd_io_complete;

	/*
	 * Find the memory pages belonging to the restored process
	 * so that it is trackable when all pages have been transferred.
	 */
	ret = collect_iovs(lpi);
	if (ret < 0)
		goto out;
	lpi->total_pages = ret;

	lp_debug(lpi, "Found %ld pages to be handled by UFFD\n", lpi->total_pages);

	list_add_tail(&lpi->l, &lpis);
	*_lpi = lpi;

	return 0;

out:
	lpi_fini(lpi);
	return -1;
}

static int handle_exit(struct lazy_pages_info *lpi)
{
	lp_debug(lpi, "EXIT\n");
	if (epoll_del_rfd(epollfd, &lpi->lpfd))
		return -1;
	free_iovs(lpi);
	close(lpi->lpfd.fd);
	lpi->lpfd.fd = -lpi->lpfd.fd;
	lpi->exited = true;

	/* keep it for tracking in-flight requests and for the summary */
	list_move_tail(&lpi->l, &lpis);

	return 0;
}

static bool uffd_recoverable_error(int mcopy_rc)
{
	if (errno == EAGAIN || errno == ENOENT || errno == EEXIST)
		return true;

	if (mcopy_rc == -ENOENT || mcopy_rc == -EEXIST)
		return true;

	return false;
}

static int uffd_check_op_error(struct lazy_pages_info *lpi, const char *op, unsigned long *nr_pages, long mcopy_rc)
{
	if (errno == ENOSPC || errno == ESRCH) {
		handle_exit(lpi);
		return 0;
	}

	if (!uffd_recoverable_error(mcopy_rc)) {
		lp_perror(lpi, "%s: mcopy_rc:%ld", op, mcopy_rc);
		return -1;
	}

	lp_debug(lpi, "%s: mcopy_rc:%ld, errno:%d\n", op, mcopy_rc, errno);

	if (mcopy_rc <= 0)
		*nr_pages = 0;
	else
		*nr_pages = mcopy_rc / PAGE_SIZE;

	return 0;
}

static int uffd_copy(struct lazy_pages_info *lpi, __u64 address, unsigned long *nr_pages)
{
	struct uffdio_copy uffdio_copy;
	unsigned long len = *nr_pages * page_size();

	uffdio_copy.dst = address;
	uffdio_copy.src = (unsigned long)lpi->buf;
	uffdio_copy.len = len;
	uffdio_copy.mode = 0;
	uffdio_copy.copy = 0;

	lp_debug(lpi, "uffd_copy: 0x%llx/%ld\n", uffdio_copy.dst, len);
	if (ioctl(lpi->lpfd.fd, UFFDIO_COPY, &uffdio_copy) &&
	    uffd_check_op_error(lpi, "copy", nr_pages, uffdio_copy.copy))
		return -1;

	lpi->copied_pages += *nr_pages;

	return 0;
}

static int uffd_io_complete(struct page_read *pr, unsigned long img_addr, unsigned long nr)
{
	struct lazy_pages_info *lpi;
	unsigned long addr = 0, req_pages;
	struct lazy_iov *req;
	int ret;

	lpi = container_of(pr, struct lazy_pages_info, pr);

	/*
	 * The process may exit while we still have requests in
	 * flight. We just drop the request and the received data in
	 * this case to avoid making uffd unhappy
	 */
	if (lpi->exited)
		return 0;

	list_for_each_entry(req, &lpi->reqs, l) {
		if (req->img_start == img_addr) {
			addr = req->start;
			break;
		}
	}

	/* the request may be already gone because if unmap/remove */
	if (!addr)
		return 0;

	/*
	 * By the time we get the pages from the remote source, parts
	 * of the request may already be gone because of unmap/remove
	 * OTOH, the remote side may send less pages than we requested.
	 * Make sure we are not trying to uffd_copy more memory than
	 * we should.
	 */
	req_pages = (req->end - req->start) / PAGE_SIZE;
	nr = min(nr, req_pages);

	ret = uffd_copy(lpi, addr, &nr);
	if (ret < 0)
		return ret;

	/* recheck if the process exited, it may be detected in uffd_copy */
	if (lpi->exited)
		return 0;

	/*
	 * Since the completed request length may differ from the
	 * actual data we've received we re-insert the request to IOVs
	 * list and let drop_iovs do the range math, free memory etc.
	 */
	iov_list_insert(req, &lpi->iovs);
	return drop_iovs(lpi, addr, nr * PAGE_SIZE);
}

static int uffd_zero(struct lazy_pages_info *lpi, __u64 address, unsigned long nr_pages)
{
	struct uffdio_zeropage uffdio_zeropage;
	unsigned long len = page_size() * nr_pages;

	uffdio_zeropage.range.start = address;
	uffdio_zeropage.range.len = len;
	uffdio_zeropage.mode = 0;

	lp_debug(lpi, "zero page at 0x%llx\n", address);
	if (ioctl(lpi->lpfd.fd, UFFDIO_ZEROPAGE, &uffdio_zeropage) &&
	    uffd_check_op_error(lpi, "zero", &nr_pages, uffdio_zeropage.zeropage))
		return -1;

	return 0;
}

/*
 * Seek for the requested address in the pagemap. If it is found, the
 * subsequent call to pr->page_read will bring us the data. If the
 * address is not found in the pagemap, but no error occurred, the
 * address should be mapped to zero pfn.
 *
 * Returns 0 for zero pages, 1 for "real" pages and negative value on
 * error
 */
static int uffd_seek_pages(struct lazy_pages_info *lpi, __u64 address, unsigned long nr)
{
	int ret;

	lpi->pr.reset(&lpi->pr);

	ret = lpi->pr.seek_pagemap(&lpi->pr, address);
	if (!ret) {
		lp_err(lpi, "no pagemap covers %llx\n", address);
		return -1;
	}

	return 0;
}

static int uffd_handle_pages(struct lazy_pages_info *lpi, __u64 address, unsigned long nr, unsigned flags)
{
	int ret;

	ret = uffd_seek_pages(lpi, address, nr);
	if (ret)
		return ret;

	ret = lpi->pr.read_pages(&lpi->pr, address, nr, lpi->buf, flags);
	if (ret <= 0) {
		lp_err(lpi, "failed reading pages at %llx\n", address);
		return ret;
	}

	return 0;
}

static struct lazy_iov *pick_next_range(struct lazy_pages_info *lpi)
{
	return list_first_entry(&lpi->iovs, struct lazy_iov, l);
}

/*
 * This is very simple heurstics for background transfer control.
 * The idea is to transfer larger chunks when there is no page faults
 * and drop the background transfer size each time #PF occurs to some
 * default value. The default is empirically set to 64Kbytes
 */
static void update_xfer_len(struct lazy_pages_info *lpi, bool pf)
{
	if (pf)
		lpi->xfer_len = DEFAULT_XFER_LEN;
	else
		lpi->xfer_len += DEFAULT_XFER_LEN;

	if (lpi->xfer_len > MAX_XFER_LEN)
		lpi->xfer_len = MAX_XFER_LEN;
}

static int xfer_pages(struct lazy_pages_info *lpi)
{
	struct lazy_iov *iov;
	unsigned long nr_pages;
	unsigned long len;
	int err;

	iov = pick_next_range(lpi);
	if (!iov)
		return 0;

	len = min(iov->end - iov->start, lpi->xfer_len);

	iov = extract_range(iov, iov->start, iov->start + len);
	if (!iov)
		return -1;
	list_move(&iov->l, &lpi->reqs);

	nr_pages = (iov->end - iov->start) / PAGE_SIZE;

	update_xfer_len(lpi, false);

	err = uffd_handle_pages(lpi, iov->img_start, nr_pages, PR_ASYNC | PR_ASAP);
	if (err < 0) {
		lp_err(lpi, "Error during UFFD copy\n");
		return -1;
	}

	return 0;
}

static int handle_remove(struct lazy_pages_info *lpi, struct uffd_msg *msg)
{
	struct uffdio_range unreg;

	unreg.start = msg->arg.remove.start;
	unreg.len = msg->arg.remove.end - msg->arg.remove.start;

	lp_debug(lpi, "%s: %llx(%llx)\n", msg->event == UFFD_EVENT_REMOVE ? "REMOVE" : "UNMAP", unreg.start, unreg.len);

	/*
	 * The REMOVE event does not change the VMA, so we need to
	 * make sure that we won't handle #PFs in the removed
	 * range. With UNMAP, there's no VMA to worry about
	 */
	if (msg->event == UFFD_EVENT_REMOVE && ioctl(lpi->lpfd.fd, UFFDIO_UNREGISTER, &unreg)) {
		/*
		 * The kernel returns -ENOMEM when unregister is
		 * called after the process has gone
		 */
		if (errno == ENOMEM) {
			handle_exit(lpi);
			return 0;
		}

		pr_perror("Failed to unregister (%llx - %llx)", unreg.start, unreg.start + unreg.len);
		return -1;
	}

	return drop_iovs(lpi, unreg.start, unreg.len);
}

static int handle_remap(struct lazy_pages_info *lpi, struct uffd_msg *msg)
{
	unsigned long from = msg->arg.remap.from;
	unsigned long to = msg->arg.remap.to;
	unsigned long len = msg->arg.remap.len;

	lp_debug(lpi, "REMAP: %lx -> %lx (%ld)\n", from, to, len);

	return remap_iovs(lpi, from, to, len);
}

static int handle_fork(struct lazy_pages_info *parent_lpi, struct uffd_msg *msg)
{
	struct lazy_pages_info *lpi;
	int uffd = msg->arg.fork.ufd;

	lp_debug(parent_lpi, "FORK: child with ufd=%d\n", uffd);

	lpi = lpi_init();
	if (!lpi)
		return -1;

	if (copy_iovs(parent_lpi, lpi))
		goto out;

	lpi->pid = parent_lpi->pid;
	lpi->lpfd.fd = uffd;
	lpi->parent = parent_lpi->parent ? parent_lpi->parent : parent_lpi;
	lpi->copied_pages = lpi->parent->copied_pages;
	lpi->total_pages = lpi->parent->total_pages;
	list_add_tail(&lpi->l, &pending_lpis);

	dup_page_read(&lpi->parent->pr, &lpi->pr);

	lpi_get(lpi->parent);

	page_read_disable_dedup(&parent_lpi->pr);
	page_read_disable_dedup(&lpi->pr);
	return 1;

out:
	lpi_fini(lpi);
	return -1;
}

/*
 * We may exit epoll_run_rfds() loop because of non-fork() event. In
 * such case we return 1 rather than 0 to let the caller know that no
 * fork() events were pending
 */
static int complete_forks(int epollfd, struct epoll_event **events, int *nr_fds)
{
	struct lazy_pages_info *lpi, *n;
	struct epoll_event *tmp;

	if (list_empty(&pending_lpis))
		return 1;

	list_for_each_entry(lpi, &pending_lpis, l)
		(*nr_fds)++;

	tmp = xrealloc(*events, sizeof(struct epoll_event) * (*nr_fds));
	if (!tmp)
		return -1;
	*events = tmp;

	list_for_each_entry_safe(lpi, n, &pending_lpis, l) {
		if (epoll_add_rfd(epollfd, &lpi->lpfd))
			return -1;

		list_del_init(&lpi->l);
		list_add_tail(&lpi->l, &lpis);
	}

	return 0;
}

static bool is_page_queued(struct lazy_pages_info *lpi, unsigned long addr)
{
	struct lazy_iov *req;

	list_for_each_entry(req, &lpi->reqs, l)
		if (addr >= req->start && addr < req->end)
			return true;

	return false;
}

static int handle_page_fault(struct lazy_pages_info *lpi, struct uffd_msg *msg)
{
	struct lazy_iov *iov;
	__u64 address;
	int ret, sidx;

	/* Align requested address to the next page boundary */
	address = msg->arg.pagefault.address & ~(page_size() - 1);
	lp_debug(lpi, "#PF at 0x%llx\n", address);

	if (is_page_queued(lpi, address))
		return 0;

	/*
	 * Stage 2c shmem zero-copy dispatch. If the fault sits in a shmem
	 * VMA managed by the streamer, either UFFDIO_CONTINUE immediately
	 * (range filled) or defer (range pending NIXL). We must NOT block
	 * the daemon's epoll thread waiting on the futex — that would
	 * deadlock handle_streamer_evfd which is responsible for flipping
	 * it. Faulting thread stays blocked in the kernel either way.
	 */
	sidx = streamer_vma_lookup(lpi, (uintptr_t)address);
	if (sidx >= 0) {
		unsigned state =
			__atomic_load_n(&streamer_ready_futex[sidx], __ATOMIC_ACQUIRE);
		if (state == 0xFFFFFFFFu)
			return uffd_zero(lpi, address, 1);
		if (state == 1)
			return uffd_continue_memfd(lpi, (uintptr_t)address, PAGE_SIZE);
		return streamer_defer_fault(sidx, lpi, (uintptr_t)address);
	}

	iov = find_iov(lpi, address);
	if (!iov)
		return uffd_zero(lpi, address, 1);

	iov = extract_range(iov, address, address + PAGE_SIZE);
	if (!iov)
		return -1;

	list_move(&iov->l, &lpi->reqs);

	update_xfer_len(lpi, true);

	ret = uffd_handle_pages(lpi, iov->img_start, 1, PR_ASYNC | PR_ASAP);
	if (ret < 0) {
		lp_err(lpi, "Error during regular page copy\n");
		return -1;
	}

	return 0;
}

static int handle_uffd_event(struct epoll_rfd *lpfd)
{
	struct lazy_pages_info *lpi;
	struct uffd_msg msg;
	int ret;

	lpi = container_of(lpfd, struct lazy_pages_info, lpfd);

	ret = read(lpfd->fd, &msg, sizeof(msg));
	if (ret < 0) {
		/* we've already handled the page fault for another thread */
		if (errno == EAGAIN)
			return 0;
		if (errno == EBADF && lpi->exited) {
			lp_debug(lpi, "excess message in queue: %d", msg.event);
			return 0;
		}
		lp_perror(lpi, "Can't read uffd message");
		return -1;
	} else if (ret == 0) {
		return 1;
	} else if (ret != sizeof(msg)) {
		lp_err(lpi, "Can't read uffd message: short read");
		return -1;
	}

	switch (msg.event) {
	case UFFD_EVENT_PAGEFAULT:
		return handle_page_fault(lpi, &msg);
	case UFFD_EVENT_REMOVE:
	case UFFD_EVENT_UNMAP:
		return handle_remove(lpi, &msg);
	case UFFD_EVENT_REMAP:
		return handle_remap(lpi, &msg);
	case UFFD_EVENT_FORK:
		return handle_fork(lpi, &msg);
	default:
		lp_err(lpi, "unexpected uffd event %u\n", msg.event);
		return -1;
	}

	return 0;
}

static void lazy_pages_summary(struct lazy_pages_info *lpi)
{
	lp_debug(lpi, "UFFD transferred pages: (%ld/%ld)\n", lpi->copied_pages, lpi->total_pages);

#if 0
	if ((lpi->copied_pages != lpi->total_pages) && (lpi->total_pages > 0)) {
		lp_warn(lpi, "Only %ld of %ld pages transferred via UFFD\n"
			"Something probably went wrong.\n",
			lpi->copied_pages, lpi->total_pages);
		return 1;
	}
#endif
}

static int handle_requests(int epollfd, struct epoll_event **events, int nr_fds)
{
	struct lazy_pages_info *lpi, *n;
	int poll_timeout = -1;
	int ret;

	for (;;) {
		ret = epoll_run_rfds(epollfd, *events, nr_fds, poll_timeout);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			ret = complete_forks(epollfd, events, &nr_fds);
			if (ret < 0)
				goto out;
			if (restore_finished)
				poll_timeout = 0;
			if (!restore_finished || !ret)
				continue;
		}

		/* make sure we return success if there is nothing to xfer */
		ret = 0;

		list_for_each_entry_safe(lpi, n, &lpis, l) {
			if (!list_empty(&lpi->iovs) && list_empty(&lpi->reqs)) {
				ret = xfer_pages(lpi);
				if (ret < 0)
					goto out;
				break;
			}

			if (list_empty(&lpi->reqs)) {
				lazy_pages_summary(lpi);
				list_del(&lpi->l);
				lpi_put(lpi);
			}
		}

		if (list_empty(&lpis))
			break;
	}

out:
	return ret;
}

int lazy_pages_finish_restore(void)
{
	uint32_t fin = LAZY_PAGES_RESTORE_FINISHED;
	int fd, ret;

	if (!opts.lazy_pages && !opts.stream_restore)
		return 0;

	fd = fdstore_get(lazy_pages_sk_id);
	if (fd < 0) {
		pr_err("No lazy-pages socket\n");
		return -1;
	}

	ret = send(fd, &fin, sizeof(fin), 0);
	if (ret != sizeof(fin))
		pr_perror("Failed sending restore finished indication");

	close(fd);

	return ret < 0 ? ret : 0;
}

static int prepare_lazy_socket(void)
{
	int listen;
	struct sockaddr_un saddr;

	if (prepare_sock_addr(&saddr))
		return -1;

	pr_debug("Waiting for incoming connections on %s\n", saddr.sun_path);
	if ((listen = server_listen(&saddr)) < 0) {
		pr_perror("server_listen error");
		return -1;
	}

	return listen;
}

static int lazy_sk_read_event(struct epoll_rfd *rfd)
{
	uint32_t fin;
	int ret;

	ret = recv(rfd->fd, &fin, sizeof(fin), 0);
	/*
	 * epoll sets POLLIN | POLLHUP for the EOF case, so we get short
	 * read just before hangup_event
	 */
	if (!ret)
		return 0;

	if (ret != sizeof(fin)) {
		pr_perror("Failed getting restore finished indication");
		return -1;
	}

	if (fin != LAZY_PAGES_RESTORE_FINISHED) {
		pr_err("Unexpected response: %x\n", fin);
		return -1;
	}

	restore_finished = true;

	return 1;
}

static int lazy_sk_hangup_event(struct epoll_rfd *rfd)
{
	if (!restore_finished) {
		pr_err("Restorer unexpectedly closed the connection\n");
		return -1;
	}

	return 0;
}

/*
 * Wake all PIE waiters with the abort sentinel so they bail out of
 * io_submit cleanly. The futex array is shmalloc'd memory shared with
 * the restorer; PIE side performs FUTEX_WAIT on each word.
 */
static void streamer_poison_futexes(void)
{
	unsigned int i;

	if (!streamer_ready_futex)
		return;

	for (i = 0; i < streamer_ready_futex_n; i++)
		__atomic_store_n(&streamer_ready_futex[i], 0xFFFFFFFFu,
				 __ATOMIC_RELEASE);

	/*
	 * Best-effort FUTEX_WAKE on each word. INT_MAX wakes any
	 * sleepers without requiring us to know exact waiter counts.
	 */
	for (i = 0; i < streamer_ready_futex_n; i++)
		syscall(SYS_futex, &streamer_ready_futex[i], FUTEX_WAKE,
			INT_MAX, NULL, NULL, 0);
}

/*
 * Per-memfd ready signal from the streamer. The eventfd counter is
 * drained on each EPOLLIN; the actual count value is ignored (we only
 * care that the streamer wrote something nonzero).
 *
 * Once a memfd is ready, the daemon would normally call
 * uffd_continue_memfd() against the corresponding shmem VMA range to
 * install the bytes via UFFDIO_CONTINUE. Wiring the VMA-range lookup
 * is deferred to a follow-on commit; here we just consume the event
 * and let any private-pages futex word matched by index transition to
 * "ready" so PIE can submit io_submit on that range.
 */
static int handle_streamer_evfd(struct epoll_rfd *rfd)
{
	uint64_t val;
	int ret;
	unsigned int idx;

	ret = read(rfd->fd, &val, sizeof(val));
	if (ret < 0) {
		if (errno == EAGAIN)
			return 0;
		pr_perror("streamer eventfd read error");
		return -1;
	}
	if (ret != sizeof(val)) {
		pr_err("streamer eventfd short read: %d\n", ret);
		return -1;
	}

	/* Find which memfd fired (linear scan; n is small). */
	idx = streamer_evfd_n;
	if (streamer_evfd_rfds) {
		unsigned int i;
		for (i = 0; i < streamer_evfd_n; i++) {
			if (streamer_evfd_rfds[i].fd == rfd->fd) {
				idx = i;
				break;
			}
		}
	}
	pr_debug("streamer evfd[%u] ready (val=%llu)\n", idx,
		 (unsigned long long)val);

	/*
	 * Mark the matching futex word ready (Stage 2c daemon-side gate).
	 * The daemon never FUTEX_WAITs on this word — handle_page_fault
	 * reads it and either fires UFFDIO_CONTINUE inline or defers via
	 * streamer_pending_faults[idx]. After flipping the word we drain
	 * the pending list so any threads that faulted before the fill
	 * completed get their CONTINUE now.
	 */
	if (streamer_ready_futex && idx < streamer_ready_futex_n) {
		__atomic_store_n(&streamer_ready_futex[idx], 1,
				 __ATOMIC_RELEASE);
		if (streamer_drain_pending((int)idx) < 0) {
			pr_err("[stream-c] drain pending faults idx=%u failed\n",
			       idx);
			return -1;
		}
	}

	return 0;
}

/*
 * Streamer-initiated abort. Two paths reach this:
 *  - graceful: streamer wrote a non-zero counter on its abort eventfd
 *    (e.g. atexit handler or SIGTERM handler). Read drains the counter.
 *  - process-death: streamer closed the eventfd (SIGKILL, OOM). The
 *    kernel delivers POLLHUP, handled by handle_abort_hangup().
 * Either way: poison every PIE futex word so io_submit bails, then
 * close the lazy-pages socket so the existing error path tears down.
 */
static int handle_abort_event(struct epoll_rfd *rfd)
{
	uint64_t val;
	int ret;

	ret = read(rfd->fd, &val, sizeof(val));
	if (ret < 0 && errno != EAGAIN) {
		pr_perror("streamer abort_fd read error");
		/* fall through to poison anyway */
	}

	pr_err("streamer reported abort (val=%llu)\n",
	       (unsigned long long)val);

	streamer_poison_futexes();
	streamer_abort_pending();

	if (lazy_pages_sk_id >= 0) {
		int sk = fdstore_get(lazy_pages_sk_id);
		if (sk >= 0) {
			shutdown(sk, SHUT_RDWR);
			close(sk);
		}
	}

	return -1;
}

static int handle_abort_hangup(struct epoll_rfd *rfd)
{
	pr_err("streamer process closed abort_fd (process died)\n");
	streamer_poison_futexes();
	streamer_abort_pending();
	return -1;
}

/*
 * Install streamer-filled bytes into [addr, addr+len) on the restored
 * process's shmem VMA via UFFDIO_CONTINUE. The kernel resolves the
 * page-cache pages from the shmem inode the memfd shares.
 *
 * Called from handle_page_fault when a shmem-VMA fault hits a range whose
 * NIXL fill is already complete, and from streamer_drain_pending when the
 * eventfd later signals readiness.
 */
static int uffd_continue_memfd(struct lazy_pages_info *lpi,
			       uintptr_t addr, size_t len)
{
	struct uffdio_continue cont;

	cont.range.start = addr;
	cont.range.len = len;
	cont.mode = 0;
	cont.mapped = 0;

	if (ioctl(lpi->lpfd.fd, UFFDIO_CONTINUE, &cont) < 0) {
		lp_perror(lpi, "UFFDIO_CONTINUE %lx/%zu", addr, len);
		return -1;
	}

	if ((size_t)cont.mapped != len) {
		lp_warn(lpi, "UFFDIO_CONTINUE partial: %lld of %zu bytes\n",
			(long long)cont.mapped, len);
		/* K-CONT-PARTIAL kill-switch territory; surface as warning
		 * for now. Per-page CONTINUE re-architecture would land in a
		 * follow-on commit if this fires in practice. */
	}

	return 0;
}

/*
 * Build streamer_evfd_vmas[idx]: list of (lpi, vma_start, vma_end) tuples
 * for each shmid the streamer manages. Walks every lpi's MmEntry, picks
 * VMA_ANON_SHARED VMAs whose shmid matches one of streamer_evfd_shmids[].
 * Hugetlb shmem is excluded (UFFD MODE_MINOR doesn't support it; the
 * agent never registers MODE_MINOR for those VMAs).
 */
static int build_streamer_evfd_vmas(void)
{
	struct lazy_pages_info *lpi;
	unsigned int total = 0;

	if (streamer_evfd_n == 0)
		return 0;

	list_for_each_entry(lpi, &lpis, l) {
		MmEntry *mm = init_mm_entry(lpi);
		size_t n;

		if (!mm)
			return -1;
		for (n = 0; n < mm->n_vmas; n++) {
			VmaEntry *vma = mm->vmas[n];
			unsigned int i;

			if (vma->shmid != 0) {
				lp_debug(lpi,
					 "[stream-c] vma %lx-%lx status=%x flags=%x shmid=0x%lx\n",
					 (unsigned long)vma->start,
					 (unsigned long)vma->end,
					 vma->status, vma->flags,
					 (unsigned long)vma->shmid);
			}
			if (!(vma->status & VMA_ANON_SHARED))
				continue;
			if (vma->flags & MAP_HUGETLB)
				continue;
			for (i = 0; i < streamer_evfd_n; i++) {
				struct streamer_evfd_vma *ev;

				if (vma->shmid != streamer_evfd_shmids[i])
					continue;
				ev = xmalloc(sizeof(*ev));
				if (!ev) {
					mm_entry__free_unpacked(mm, NULL);
					return -1;
				}
				ev->lpi = lpi;
				ev->start = (uintptr_t)vma->start;
				ev->end = (uintptr_t)vma->end;
				INIT_LIST_HEAD(&ev->l);
				list_add_tail(&ev->l, &streamer_evfd_vmas[i]);
				total++;
				lp_debug(lpi,
					 "[stream-c] shmid=0x%lx idx=%u vma=%lx-%lx\n",
					 (unsigned long)vma->shmid, i,
					 (unsigned long)vma->start,
					 (unsigned long)vma->end);
			}
		}
		mm_entry__free_unpacked(mm, NULL);
	}

	pr_info("[stream-c] streamer_evfd_vmas built: %u shmem VMA(s) across %u shmid(s)\n",
		total, streamer_evfd_n);
	return 0;
}

/*
 * O(n) lookup over (typically) <50 entries. Returns the matching evfd
 * index, or -1 if the fault is not in a streamer-managed shmem VMA.
 */
static int streamer_vma_lookup(struct lazy_pages_info *lpi, uintptr_t addr)
{
	unsigned int i;

	if (streamer_evfd_n == 0)
		return -1;
	for (i = 0; i < streamer_evfd_n; i++) {
		struct streamer_evfd_vma *ev;

		list_for_each_entry(ev, &streamer_evfd_vmas[i], l) {
			if (ev->lpi != lpi)
				continue;
			if (addr >= ev->start && addr < ev->end)
				return (int)i;
		}
	}
	return -1;
}

/*
 * Defer a fault until the streamer signals range idx ready. The faulting
 * thread stays blocked in the kernel (UFFD contract); we will respond
 * with UFFDIO_CONTINUE from streamer_drain_pending when the eventfd fires.
 */
static int streamer_defer_fault(int idx, struct lazy_pages_info *lpi,
				uintptr_t addr)
{
	struct streamer_pending_fault *pf;

	pf = xmalloc(sizeof(*pf));
	if (!pf)
		return -1;
	pf->lpi = lpi;
	pf->addr = addr;
	INIT_LIST_HEAD(&pf->l);
	list_add_tail(&pf->l, &streamer_pending_faults[idx]);
	lp_debug(lpi, "[stream-c] deferred fault idx=%d addr=0x%lx\n",
		 idx, (unsigned long)addr);
	return 0;
}

/* Drain pending_faults[idx] via UFFDIO_CONTINUE. Called once per range
 * after the streamer signals readiness. */
static int streamer_drain_pending(int idx)
{
	struct streamer_pending_fault *pf, *tmp;

	if (!streamer_pending_faults)
		return 0;
	list_for_each_entry_safe(pf, tmp, &streamer_pending_faults[idx], l) {
		int rc = uffd_continue_memfd(pf->lpi, pf->addr, PAGE_SIZE);
		list_del(&pf->l);
		xfree(pf);
		if (rc < 0)
			return -1;
	}
	return 0;
}

/* Drain every pending list with uffd_zero so blocked faulting threads
 * unblock under abort. Subsequent reads will see zeros for the unfilled
 * pages, but the restore is already failing — this is a best-effort
 * cleanup path. */
static void streamer_abort_pending(void)
{
	unsigned int i;

	if (!streamer_pending_faults)
		return;
	for (i = 0; i < streamer_evfd_n; i++) {
		struct streamer_pending_fault *pf, *tmp;

		list_for_each_entry_safe(pf, tmp, &streamer_pending_faults[i], l) {
			(void)uffd_zero(pf->lpi, pf->addr, 1);
			list_del(&pf->l);
			xfree(pf);
		}
	}
}

static int prepare_uffds(int listen, int epollfd)
{
	int i;
	int client;
	socklen_t len;
	struct sockaddr_un saddr;

	/* accept new client request */
	len = sizeof(struct sockaddr_un);
	if ((client = accept(listen, (struct sockaddr *)&saddr, &len)) < 0) {
		pr_perror("server_accept error");
		close(listen);
		return -1;
	}

	for (i = 0; i < task_entries->nr_tasks; i++) {
		struct lazy_pages_info *lpi = NULL;
		if (ud_open(client, &lpi))
			goto close_uffd;
		if (lpi == NULL)
			continue;
		if (epoll_add_rfd(epollfd, &lpi->lpfd))
			goto close_uffd;
	}

	lazy_sk_rfd.fd = client;
	lazy_sk_rfd.read_event = lazy_sk_read_event;
	lazy_sk_rfd.hangup_event = lazy_sk_hangup_event;
	if (epoll_add_rfd(epollfd, &lazy_sk_rfd))
		goto close_uffd;

	/*
	 * Pipeline C: wire the streamer abort_fd and per-memfd ready
	 * eventfds into the daemon epoll set. The fds themselves arrive
	 * from the agent via a separate handshake (added in a follow-on
	 * commit); until that handshake runs, streamer_abort_rfd.fd stays
	 * -1 and streamer_evfd_rfds stays NULL, so this block is a no-op
	 * in the default-path build.
	 */
	if (opts.stream_restore) {
		unsigned int i;

		if (streamer_abort_rfd.fd >= 0) {
			streamer_abort_rfd.read_event = handle_abort_event;
			streamer_abort_rfd.hangup_event = handle_abort_hangup;
			if (epoll_add_rfd(epollfd, &streamer_abort_rfd))
				goto close_uffd;
		}

		for (i = 0; i < streamer_evfd_n; i++) {
			streamer_evfd_rfds[i].read_event = handle_streamer_evfd;
			if (epoll_add_rfd(epollfd, &streamer_evfd_rfds[i]))
				goto close_uffd;
		}
	}

	close(listen);
	return 0;

close_uffd:
	close_safe(&client);
	close(listen);
	return -1;
}

int cr_lazy_pages(bool daemon)
{
	struct epoll_event *events = NULL;
	int nr_fds;
	int lazy_sk;
	int ret;

	if (!kdat.has_uffd)
		return -1;

	if (prepare_dummy_pstree())
		return -1;

	lazy_sk = prepare_lazy_socket();
	if (lazy_sk < 0)
		return -1;

	if (daemon) {
		ret = cr_daemon(1, 0, -1);
		if (ret == -1) {
			pr_err("Can't run in the background\n");
			return -1;
		}
		if (ret > 0) { /* parent task, daemon started */
			if (opts.pidfile) {
				if (write_pidfile(ret) == -1) {
					pr_perror("Can't write pidfile");
					kill(ret, SIGKILL);
					waitpid(ret, NULL, 0);
					return -1;
				}
			}

			return 0;
		}
	}

	if (status_ready())
		return -1;

	if (recv_streamer_daemon_fds())
		return -1;

	/*
	 * we poll nr_tasks userfault fds, UNIX socket between lazy-pages
	 * daemon and the cr-restore, and, optionally TCP socket for
	 * remote pages. In stream-restore (Pipeline C) mode also reserve
	 * room for the streamer abort_fd and per-memfd ready eventfds
	 * (populated by recv_streamer_daemon_fds above).
	 */
	nr_fds = task_entries->nr_tasks + (opts.use_page_server ? 2 : 1);
	if (opts.stream_restore)
		nr_fds += (streamer_abort_rfd.fd >= 0 ? 1 : 0) + streamer_evfd_n;
	epollfd = epoll_prepare(nr_fds, &events);
	if (epollfd < 0)
		return -1;

	if (prepare_uffds(lazy_sk, epollfd)) {
		xfree(events);
		return -1;
	}

	/*
	 * Stage 2c: now that lpis are populated, walk every MmEntry to
	 * resolve the (shmid -> VMA list) mapping the on-fault CONTINUE
	 * dispatcher needs. No-op if recv_streamer_daemon_fds didn't run
	 * (private-only restore).
	 */
	if (opts.stream_restore && streamer_evfd_n > 0) {
		if (build_streamer_evfd_vmas()) {
			xfree(events);
			return -1;
		}
	}

	if (opts.use_page_server) {
		if (connect_to_page_server_to_recv(epollfd)) {
			xfree(events);
			return -1;
		}
	}

	ret = handle_requests(epollfd, &events, nr_fds);

	disconnect_from_page_server();

	xfree(events);
	return ret;
}
