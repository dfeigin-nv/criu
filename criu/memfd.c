#include <unistd.h>
#include <linux/memfd.h>

#include "common/compiler.h"
#include "memfd.h"
#include "fdinfo.h"
#include "imgset.h"
#include "image.h"
#include "util.h"
#include "log.h"
#include "files.h"
#include "fs-magic.h"
#include "kerndat.h"
#include "files-reg.h"
#include "rst-malloc.h"
#include "fdstore.h"
#include "file-ids.h"
#include "namespaces.h"
#include "shmem.h"
#include "hugetlb.h"
#include "worker-pool.h"

#include "protobuf.h"
#include "images/memfd.pb-c.h"

#define MEMFD_PREFIX	 "/memfd:"
#define MEMFD_PREFIX_LEN (sizeof(MEMFD_PREFIX) - 1)

#define F_SEAL_SEAL   0x0001 /* prevent further seals from being set */
#define F_SEAL_SHRINK 0x0002 /* prevent file from shrinking */
#define F_SEAL_GROW   0x0004 /* prevent file from growing */
#define F_SEAL_WRITE  0x0008 /* prevent writes */
/* Linux 5.1+ */
#define F_SEAL_FUTURE_WRITE 0x0010 /* prevent future writes while mapped */

struct memfd_dump_inode {
	struct list_head list;
	u32 id;
	u32 dev;
	u32 ino;
};

struct memfd_restore_inode {
	struct list_head list;
	int fdstore_id;
	unsigned int pending_seals;
	MemfdInodeEntry *mie;
	bool was_opened_rw;
};

static LIST_HEAD(memfd_inodes);

/*
 * Dump only
 */

static u32 memfd_inode_ids = 1;

int is_memfd(dev_t dev)
{
	return dev == kdat.shmem_dev;
}

static int dump_memfd_inode(int fd, struct memfd_dump_inode *inode, const char *name, const struct stat *st)
{
	MemfdInodeEntry mie = MEMFD_INODE_ENTRY__INIT;
	int ret = -1, flag;
	u32 shmid;

	/*
	  * shmids are chosen as the inode number of the corresponding mmapped
	  * file. See handle_vma() in proc_parse.c.
	  * It works for memfd too, because we share the same device as the
	  * shmem device.
	  */
	shmid = inode->ino;

	pr_info("Dumping memfd:%s contents (id %#x, shmid: %#x, size: %" PRIu64 ")\n", name, inode->id, shmid,
		st->st_size);

	if (dump_one_memfd_shmem(fd, shmid, st->st_size) < 0)
		goto out;

	mie.inode_id = inode->id;
	mie.uid = userns_uid(st->st_uid);
	mie.gid = userns_gid(st->st_gid);
	mie.name = (char *)name;
	mie.size = st->st_size;
	mie.shmid = shmid;
	if (is_hugetlb_dev(inode->dev, &flag)) {
		mie.has_hugetlb_flag = true;
		mie.hugetlb_flag = flag | MFD_HUGETLB;
	}
	mie.mode = st->st_mode;
	mie.has_mode = true;

	mie.seals = fcntl(fd, F_GET_SEALS);
	if (mie.seals == -1) {
		if (errno != EINVAL || ~mie.hugetlb_flag & MFD_HUGETLB) {
			pr_perror("fcntl(F_GET_SEALS)");
			goto out;
		}
		/* Kernels before 4.16 don't allow MFD_HUGETLB |
		 * MFD_ALLOW_SEALING and return EINVAL for
		 * fcntl(MFD_HUGETLB-enabled fd).
		 */
		mie.seals = F_SEAL_SEAL;
	}

	if (pb_write_one(img_from_set(glob_imgset, CR_FD_MEMFD_INODE), &mie, PB_MEMFD_INODE))
		goto out;

	ret = 0;

out:
	return ret;
}

static struct memfd_dump_inode *dump_unique_memfd_inode(int lfd, const char *name, const struct stat *st)
{
	struct memfd_dump_inode *inode;
	int fd;

	list_for_each_entry(inode, &memfd_inodes, list)
		if ((inode->dev == st->st_dev) && (inode->ino == st->st_ino))
			return inode;

	inode = xmalloc(sizeof(*inode));
	if (inode == NULL)
		return NULL;

	inode->dev = st->st_dev;
	inode->ino = st->st_ino;
	inode->id = memfd_inode_ids++;

	fd = open_proc(PROC_SELF, "fd/%d", lfd);
	if (fd < 0) {
		xfree(inode);
		return NULL;
	}

	if (dump_memfd_inode(fd, inode, name, st)) {
		close(fd);
		xfree(inode);
		return NULL;
	}
	close(fd);

	list_add_tail(&inode->list, &memfd_inodes);

	return inode;
}

static int dump_one_memfd(int lfd, u32 id, const struct fd_parms *p)
{
	MemfdFileEntry mfe = MEMFD_FILE_ENTRY__INIT;
	FileEntry fe = FILE_ENTRY__INIT;
	struct memfd_dump_inode *inode;
	struct fd_link _link, *link;
	const char *name;

	if (!p->link) {
		if (fill_fdlink(lfd, p, &_link))
			return -1;
		link = &_link;
	} else
		link = p->link;

	link_strip_deleted(link);
	/* link->name is always started with "." which has to be skipped.  */
	if (strncmp(link->name + 1, MEMFD_PREFIX, MEMFD_PREFIX_LEN) == 0)
		name = &link->name[1 + MEMFD_PREFIX_LEN];
	else
		name = link->name + 1;

	inode = dump_unique_memfd_inode(lfd, name, &p->stat);
	if (!inode)
		return -1;

	mfe.id = id;
	mfe.flags = p->flags;
	mfe.pos = p->pos;
	mfe.fown = (FownEntry *)&p->fown;
	mfe.inode_id = inode->id;

	fe.type = FD_TYPES__MEMFD;
	fe.id = mfe.id;
	fe.memfd = &mfe;

	return pb_write_one(img_from_set(glob_imgset, CR_FD_FILES), &fe, PB_FILE);
}

int dump_one_memfd_cond(int lfd, u32 *id, struct fd_parms *parms)
{
	if (fd_id_generate_special(parms, id))
		return dump_one_memfd(lfd, *id, parms);
	return 0;
}

const struct fdtype_ops memfd_dump_ops = {
	.type = FD_TYPES__MEMFD,
	.dump = dump_one_memfd,
};

/*
 * Restore only
 */

struct memfd_info {
	MemfdFileEntry *mfe;
	struct file_desc d;
	struct memfd_restore_inode *inode;
};

static struct memfd_restore_inode *memfd_alloc_inode(int id)
{
	struct memfd_restore_inode *inode;

	list_for_each_entry(inode, &memfd_inodes, list)
		if (inode->mie->inode_id == id)
			return inode;

	pr_err("Unable to find the %d memfd inode\n", id);
	return NULL;
}

static int collect_one_memfd_inode(void *o, ProtobufCMessage *base, struct cr_img *i)
{
	MemfdInodeEntry *mie = pb_msg(base, MemfdInodeEntry);
	struct memfd_restore_inode *inode = o;

	inode->mie = mie;
	inode->fdstore_id = -1;
	inode->pending_seals = 0;
	inode->was_opened_rw = false;

	list_add_tail(&inode->list, &memfd_inodes);

	return 0;
}

static struct collect_image_info memfd_inode_cinfo = {
	.fd_type = CR_FD_MEMFD_INODE,
	.pb_type = PB_MEMFD_INODE,
	.priv_size = sizeof(struct memfd_restore_inode),
	.collect = collect_one_memfd_inode,
	.flags = COLLECT_SHARED | COLLECT_NOFREE,
};

int prepare_memfd_inodes(void)
{
	return collect_image(&memfd_inode_cinfo);
}

/*
 * Coordinator-hoisted async memfd content restore.
 *
 * The straightforward path restores each memfd's shared-memory content lazily
 * inside the forked task (open_vmas -> memfd_open -> restore_memfd_shmem_
 * content), which dominates restore time for workloads with many large memfd
 * mappings (GPU/vLLM). Instead, do the cheap structural half (memfd_create +
 * ftruncate + fdstore_add) in the coordinator before fork, and the expensive
 * content half (mmap + page copy) in parallel on a pool of coordinator worker
 * threads, run after the per-task RESTORE stage and before sealing. The memfd
 * inode is shared via the fdstore, so once the pool drains every referencing
 * task sees the filled content through its inherited fd.
 */

/*
 * Upper bound on fill workers. The workers share the bfd buffer pool
 * (serialized by bufs_lock, see "bfd: serialize buffer pool access for
 * parallel restore") and the fill is storage-bandwidth bound, so beyond this
 * more threads stop helping. 16 matches the measured NFS nconnect knee (and the
 * cap the earlier per-task memfd-open approach settled on).
 */
#define MEMFD_RESTORE_MAX_WORKERS 16

struct memfd_restore_data_job {
	struct memfd_restore_inode *inode;
	int fd;
	bool failed;
	int saved_errno;
};

static struct memfd_restore_data_job *memfd_jobs;
static int memfd_nr_jobs;

/*
 * Async worker body: fill one memfd's content and set its permissions. Runs on
 * a worker thread, so the body itself does not log: it records failure in the
 * job and the coordinator reports it after the pool drains. The helpers it calls
 * (shmem fill, page-read, cr_fch*) can still log, and the log buffer is not
 * lock-protected, so with multiple workers those lines may interleave -- common
 * at high verbosity (-v4), where page-read emits per-entry debug output. This is
 * cosmetic only: it garbles log text, not restore state.
 */
static int memfd_restore_data(void *arg)
{
	struct memfd_restore_data_job *j = arg;
	MemfdInodeEntry *mie = j->inode->mie;
	int ret;

	ret = memfd_shmem_fill_content(j->fd, mie->shmid, mie->size);
	if (ret == 0) {
		if (mie->has_mode)
			ret = cr_fchperm(j->fd, mie->uid, mie->gid, mie->mode);
		else
			ret = cr_fchown(j->fd, mie->uid, mie->gid);
		if (ret)
			ret = -1;
	}

	if (ret < 0) {
		j->saved_errno = errno;
		j->failed = true;
	}

	close(j->fd);
	return ret;
}

static int memfd_nr_workers(int nr_jobs)
{
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	int n = (ncpu > 0) ? (int)ncpu : 1;

	if (n > MEMFD_RESTORE_MAX_WORKERS)
		n = MEMFD_RESTORE_MAX_WORKERS;
	if (n > nr_jobs)
		n = nr_jobs;
	if (n < 1)
		n = 1;

	return n;
}

/*
 * Pre-fork: for each memfd inode create the fd, set its size, register it in
 * the fdstore (so children inherit it and short-circuit memfd_open_inode via
 * fdstore_id), and queue the content fill. No worker threads are started here:
 * that must wait until after the FORKING barrier (see worker-pool.h).
 */
int memfd_content_prepare(void)
{
	struct memfd_restore_inode *inode;
	int n = 0, i = 0;

	list_for_each_entry(inode, &memfd_inodes, list)
		n++;

	if (n == 0)
		return 0;

	memfd_jobs = xzalloc(n * sizeof(*memfd_jobs));
	if (!memfd_jobs)
		return -1;

	list_for_each_entry(inode, &memfd_inodes, list) {
		MemfdInodeEntry *mie = inode->mie;
		struct memfd_restore_data_job *j = &memfd_jobs[i];
		int fd, flags;

		/*
		 * F_SEAL_SEAL means no further seals may be added, so create
		 * the memfd without MFD_ALLOW_SEALING. Otherwise allow sealing
		 * and defer the seals to apply_memfd_seals(), since
		 * F_SEAL_FUTURE_WRITE must be applied only after every writable
		 * mapping has been opened.
		 */
		if (mie->seals == F_SEAL_SEAL) {
			inode->pending_seals = 0;
			flags = 0;
		} else {
			inode->pending_seals = mie->seals;
			flags = MFD_ALLOW_SEALING;
		}

		if (mie->has_hugetlb_flag)
			flags |= mie->hugetlb_flag;

		fd = memfd_create(mie->name, flags);
		if (fd < 0) {
			pr_perror("Can't create memfd:%s", mie->name);
			return -1;
		}

		/* Structural half now (synchronous, pre-fork). */
		if (memfd_shmem_set_size(fd, mie->shmid, mie->size) < 0) {
			close(fd);
			return -1;
		}

		/*
		 * fdstore_add must happen before fork so every child inherits
		 * the registered fd and sees inode->fdstore_id != -1. It dups
		 * the fd into the store, so the worker closing its own working
		 * fd later leaves the children's fdstore_get() unaffected.
		 */
		inode->fdstore_id = fdstore_add(fd);
		if (inode->fdstore_id < 0) {
			close(fd);
			return -1;
		}

		j->inode = inode;
		j->fd = fd;

		if (worker_pool_queue(memfd_restore_data, j) < 0) {
			close(fd);
			return -1;
		}

		i++;
	}

	memfd_nr_jobs = i;
	return 0;
}

/*
 * Start the fill pool. Call after the FORKING barrier. Self-sizes the worker
 * count from the online CPUs and the queued job count.
 */
int memfd_content_start(void)
{
	if (memfd_nr_jobs == 0)
		return 0;

	return worker_pool_start(memfd_nr_workers(memfd_nr_jobs));
}

/* Wait for the fill pool to finish. Must run before apply_memfd_seals(). */
int memfd_content_drain(void)
{
	int ret, i;

	if (memfd_nr_jobs == 0)
		return 0;

	ret = worker_pool_join();

	/* Workers don't log; report any failed jobs here, after the join. */
	for (i = 0; i < memfd_nr_jobs; i++) {
		struct memfd_restore_data_job *j = &memfd_jobs[i];
		MemfdInodeEntry *mie = j->inode->mie;

		if (!j->failed)
			continue;

		errno = j->saved_errno;
		pr_perror("Can't restore memfd:%s content (shmid %#lx)", mie->name, (unsigned long)mie->shmid);
	}

	memfd_content_fini();

	return ret;
}

/*
 * Free the async fill pool and the job table. Idempotent and null-safe: runs
 * from drain() on the success path, and again from the restore abort path,
 * where prepare() may have created the pool but it was never started.
 */
void memfd_content_fini(void)
{
	worker_pool_cleanup();
	xfree(memfd_jobs);
	memfd_jobs = NULL;
	memfd_nr_jobs = 0;
}

/*
 * Every inode's content fd is created and registered in the fdstore before
 * fork by memfd_content_prepare(), so opening an inode is just fetching that
 * fd. The async fill pool is drained before any task runs, so the content is
 * already in place by the time a task opens the memfd.
 */
static int memfd_open_inode(struct memfd_restore_inode *inode)
{
	return fdstore_get(inode->fdstore_id);
}

int memfd_open(struct file_desc *d, u32 *fdflags, bool filemap)
{
	struct memfd_info *mfi;
	MemfdFileEntry *mfe;
	int fd, _fd;
	u32 flags;

	mfi = container_of(d, struct memfd_info, d);
	mfe = mfi->mfe;

	pr_info("Restoring memfd id=%d\n", mfe->id);

	fd = memfd_open_inode(mfi->inode);
	if (fd < 0)
		return -1;

	/* Reopen the fd with original permissions */
	flags = fdflags ? *fdflags : mfe->flags;

	if (filemap && (flags & O_ACCMODE) == O_RDWR)
		return fd;

	if (!mfi->inode->was_opened_rw && (flags & O_ACCMODE) == O_RDWR) {
		/*
		 * If there is only a single RW-opened fd for a memfd, it can
		 * be used to pass it to execveat() with AT_EMPTY_PATH to have
		 * its contents executed.  This currently works only for the
		 * original fd from memfd_create() so return the original fd
		 * once -- in case the caller expects to be the sole opener
		 * and does execveat() from this memfd.
		 */
		if (!fcntl(fd, F_SETFL, flags)) {
			mfi->inode->was_opened_rw = true;
			return fd;
		}

		pr_pwarn("Can't change fd flags to %#o for memfd id=%d", flags, mfe->id);
	}

	/*
	 * Ideally we should call compat version open() to not force the
	 * O_LARGEFILE file flag with regular open(). It doesn't seem that
	 * important though.
	 */
	_fd = __open_proc(PROC_SELF, 0, flags, "fd/%d", fd);
	if (_fd < 0)
		pr_perror("Can't reopen memfd id=%d", mfe->id);
	else if (!filemap && (flags & O_ACCMODE) == O_RDWR)
		pr_warn("execveat(fd=%d, ..., AT_EMPTY_PATH) might fail after restore; memfd id=%d\n", _fd, mfe->id);

	close(fd);
	return _fd;
}

static int memfd_open_fe_fd(struct file_desc *d, int *new_fd)
{
	MemfdFileEntry *mfe;
	int fd;

	if (inherited_fd(d, new_fd))
		return 0;

	fd = memfd_open(d, NULL, false);
	if (fd < 0)
		return -1;

	mfe = container_of(d, struct memfd_info, d)->mfe;

	if (restore_fown(fd, mfe->fown) < 0)
		goto err;

	if (lseek(fd, mfe->pos, SEEK_SET) < 0) {
		pr_perror("Can't restore file position of %d for memfd id=%d", fd, mfe->id);
		goto err;
	}

	*new_fd = fd;
	return 0;

err:
	close(fd);
	return -1;
}

static char *memfd_d_name(struct file_desc *d, char *buf, size_t s)
{
	MemfdInodeEntry *mie = NULL;
	struct memfd_info *mfi;

	mfi = container_of(d, struct memfd_info, d);

	mie = mfi->inode->mie;
	if (snprintf(buf, s, "%s%s", MEMFD_PREFIX, mie->name) >= s) {
		pr_err("Buffer too small for memfd name %s\n", mie->name);
		return NULL;
	}

	return buf;
}

static struct file_desc_ops memfd_desc_ops = {
	.type = FD_TYPES__MEMFD,
	.open = memfd_open_fe_fd,
	.name = memfd_d_name,
};

static int collect_one_memfd(void *o, ProtobufCMessage *msg, struct cr_img *i)
{
	struct memfd_info *info = o;

	info->mfe = pb_msg(msg, MemfdFileEntry);
	info->inode = memfd_alloc_inode(info->mfe->inode_id);
	if (!info->inode)
		return -1;

	return file_desc_add(&info->d, info->mfe->id, &memfd_desc_ops);
}

struct collect_image_info memfd_cinfo = {
	.fd_type = CR_FD_MEMFD_FILE,
	.pb_type = PB_MEMFD_FILE,
	.priv_size = sizeof(struct memfd_info),
	.collect = collect_one_memfd,
};

struct file_desc *collect_memfd(u32 id)
{
	struct file_desc *fdesc;

	fdesc = find_file_desc_raw(FD_TYPES__MEMFD, id);
	if (fdesc == NULL)
		pr_err("No entry for memfd %#x\n", id);

	return fdesc;
}

int apply_memfd_seals(void)
{
	/*
	 * We apply the seals after all the mappings are done because the seal
	 * F_SEAL_FUTURE_WRITE prevents future write access (added in
	 * Linux 5.1). Thus we must make sure all writable mappings are opened
	 * before applying this seal.
	 */

	int ret, fd;
	struct memfd_restore_inode *inode;

	list_for_each_entry(inode, &memfd_inodes, list) {
		if (!inode->pending_seals)
			continue;

		fd = memfd_open_inode(inode);
		if (fd < 0)
			return -1;

		ret = fcntl(fd, F_ADD_SEALS, inode->pending_seals);
		close(fd);

		if (ret < 0) {
			pr_perror("Cannot apply seals on memfd");
			return -1;
		}
	}

	return 0;
}
