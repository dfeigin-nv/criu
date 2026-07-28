#include <unistd.h>
#include <linux/memfd.h>

#include "common/compiler.h"
#include "common/lock.h"
#include "cr_options.h"
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
#include "memfd-cache.h"
#include "file-ids.h"
#include "namespaces.h"
#include "asyncd.h"
#include "shmem.h"
#include "hugetlb.h"
#include "cr_options.h"
#include "servicefd.h"

#include "protobuf.h"
#include "images/memfd.pb-c.h"

/* Pipeline C Stage 2c: defined in mem.c, used here to adopt the
 * streamer-owned memfd as the backing inode for memfd-restored
 * shmem so the lazy-pages daemon UFFDIO_CONTINUE-installs from the
 * same shmem page cache the streamer fills. */
extern int recv_streamer_shmem_memfd(uint64_t shmid, int *out_memfd);

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
	mutex_t lock;
	int fdstore_id;
	unsigned int pending_seals;
	MemfdInodeEntry *mie;
	bool was_opened_rw;
	bool cache_donate; /* memfd-cache MISS: donate this sealed fd after restore */
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
	mutex_init(&inode->lock);
	inode->fdstore_id = -1;
	inode->pending_seals = 0;
	inode->was_opened_rw = false;
	inode->cache_donate = false;

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

static int memfd_open_inode_nocache(struct memfd_restore_inode *inode)
{
	MemfdInodeEntry *mie = NULL;
	int fd = -1;
	int ret, exit_code = -1;
	int flags;
	bool streamer_owned = false;

	mie = inode->mie;

	/*
	 * Node-local memfd content cache (opt-in via the snapshot-agent; the
	 * socket fd rides CRIU_MEMFD_CACHE_SOCK, scoped by memfd_cache_id). If a
	 * previous restore on this node already filled and sealed this inode,
	 * borrow the sealed fd instead of re-creating, re-filling and re-sealing.
	 * A HIT fd is kernel-state-identical to what the fill+seal path produces
	 * (sealed, sized, correctly-owned -- uid/gid are in the cache key), so
	 * every downstream open/seal/mapping step is unchanged; we skip the fill,
	 * the (later) seal, and the (already-correct) chown. MISS fills now and
	 * donates the sealed fd after restore; a broken socket just fills locally.
	 */
	if (opts.memfd_cache && opts.memfd_cache_id && opts.memfd_cache_id[0] && memfd_cache_eligible(mie)) {
		int cache_sock = memfd_cache_sock();
		int cfd;

		if (cache_sock >= 0) {
			switch (memfd_cache_get(cache_sock, mie, opts.memfd_cache_id, &cfd)) {
			case MEMFD_CACHE_R_HIT:
				inode->fdstore_id = fdstore_add(cfd);
				if (inode->fdstore_id < 0) {
					close(cfd);
					return -1;
				}
				inode->pending_seals = 0;
				pr_debug("Borrowed cached memfd:%s (shmid %#lx)\n", mie->name, (unsigned long)mie->shmid);
				return cfd;
			case MEMFD_CACHE_R_MISS:
				inode->cache_donate = true;
				break;
			case MEMFD_CACHE_R_ERR:
			default:
				break;
			}
		}
	}

	if (mie->seals == F_SEAL_SEAL) {
		inode->pending_seals = 0;
		flags = 0;
	} else {
		/* Seals are applied later due to F_SEAL_FUTURE_WRITE */
		inode->pending_seals = mie->seals;
		flags = MFD_ALLOW_SEALING;
	}

	if (mie->has_hugetlb_flag)
		flags |= mie->hugetlb_flag;

	/*
	 * Stage 2c: adopt the streamer-owned memfd as the backing file so the
	 * lazy-pages daemon's UFFDIO_CONTINUE installs page cache pages from
	 * the same shmem inode the streamer fills. Hugetlb shmem is excluded
	 * since UFFD MODE_MINOR doesn't support it; fall through to the normal
	 * memfd_create + fill path in that case.
	 */
	if (opts.stream_restore && !mie->has_hugetlb_flag &&
	    get_service_fd(STREAM_SHMEM_SK_OFF) >= 0) {
		if (recv_streamer_shmem_memfd(mie->shmid, &fd) < 0) {
			pr_err("Failed to recv shmem memfd from streamer for shmid=0x%lx\n",
			       (unsigned long)mie->shmid);
			goto err;
		}
		streamer_owned = true;
	} else {
		fd = memfd_create(mie->name, flags);
		if (fd < 0) {
			pr_perror("Can't create memfd:%s", mie->name);
			goto err;
		}
	}

	if (ftruncate(fd, mie->size) < 0) {
		pr_perror("Can't resize shmem 0x%x size=%" PRIu64, mie->shmid, mie->size);
		goto err;
	}

	if (streamer_owned && !(mie->seals & (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE))) {
		/*
		 * This inode came from the streamer, so leave it empty: it is
		 * already sized by the ftruncate() above and the streamer
		 * fills the very same inode out-of-band. Where a lazy-pages
		 * daemon is present the bytes are additionally installed on
		 * demand via UFFDIO_CONTINUE.
		 *
		 * The skip is gated on streamer_owned, NOT on
		 * opts.stream_restore. A provider may stream private ranges
		 * only and serve no shmem at all -- dynamo's
		 * writePipelineCManifest emits "shmem_ranges":[] today -- in
		 * which case STREAM_SHMEM_SK_OFF is absent, the memfd above is
		 * one CRIU created itself, and nobody else will ever write to
		 * it. Skipping the fill there leaves the restored process with
		 * a zero-filled shmem region and it dies with SIGSEGV as soon
		 * as it touches its own data.
		 *
		 * Write-sealed inodes are excluded: apply_memfd_seals() runs
		 * before the tasks resume, so the seal would land before the
		 * streamer finished writing and its writes would fail EPERM.
		 * Those fall through to the conventional fill below.
		 */
		pr_debug("stream-restore: deferring fill of memfd:%s (shmid %#lx) to streamer\n",
			 mie->name, (unsigned long)mie->shmid);
	} else if (opts.stream) {
		/*
		 * criu-image-streamer serves the image in a single sequential
		 * pass and does not reopen it. The async fill daemon reads the
		 * memfd content out-of-band from a separate process, which
		 * breaks that contract, so fill inline when restoring from a
		 * stream.
		 */
		if (restore_shmem_fd_content(fd, mie->shmid, mie->size))
			goto err;
	} else {
		struct async_restore_shmem_args async_arg = {
			.shmid = mie->shmid,
			.size = mie->size,
		};

		if (streamer_owned)
			pr_warn("stream-restore: memfd:%s (shmid %#lx) is write-sealed; "
				"filling from image instead of streaming\n",
				mie->name, (unsigned long)mie->shmid);

		if (async_call(async_restore_shmem_content, 0,
			       &async_arg, sizeof(async_arg), fd))
			goto err;
	}

	if (mie->has_mode)
		ret = cr_fchperm(fd, mie->uid, mie->gid, mie->mode);
	else
		ret = cr_fchown(fd, mie->uid, mie->gid);
	if (ret) {
		pr_perror("Can't set permissions { uid %d gid %d mode %#o } of memfd:%s", (int)mie->uid,
			  (int)mie->gid, mie->has_mode ? (int)mie->mode : -1, mie->name);
		goto err;
	}

	inode->fdstore_id = fdstore_add(fd);
	if (inode->fdstore_id < 0)
		goto err;

	exit_code = fd;
	fd = -1;

err:
	close_safe(&fd);
	return exit_code;
}

static int memfd_open_inode(struct memfd_restore_inode *inode)
{
	int fd;

	if (inode->fdstore_id != -1)
		return fdstore_get(inode->fdstore_id);

	mutex_lock(&inode->lock);
	if (inode->fdstore_id != -1)
		fd = fdstore_get(inode->fdstore_id);
	else
		fd = memfd_open_inode_nocache(inode);
	mutex_unlock(&inode->lock);

	return fd;
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

int memfd_content_donate(void)
{
	struct memfd_restore_inode *inode;
	const char *cache_id;
	int cache_sock;

	if (!opts.memfd_cache || !opts.memfd_cache_id || !opts.memfd_cache_id[0])
		return 0;

	cache_sock = memfd_cache_sock();
	if (cache_sock < 0)
		return 0;
	cache_id = opts.memfd_cache_id;

	/*
	 * Runs after apply_memfd_seals(), so every cache_donate inode now holds a
	 * sealed (F_SEAL_FUTURE_WRITE) populated fd -- the final artifact a future
	 * HIT will borrow. Donation is pure cache population: the restore has
	 * already succeeded, so a failure here is logged and never propagated.
	 */
	list_for_each_entry(inode, &memfd_inodes, list) {
		int fd;

		if (!inode->cache_donate)
			continue;

		fd = memfd_open_inode(inode);
		if (fd < 0) {
			pr_warn("memfd-cache: can't open inode to donate (shmid %#lx)\n",
				(unsigned long)inode->mie->shmid);
			break;
		}

		if (memfd_cache_donate(cache_sock, fd, inode->mie, cache_id) < 0) {
			/* Broken socket: stop, but never fail the restore. */
			close(fd);
			break;
		}
		close(fd);
	}

	return 0;
}

/*
 * Eager cache primer (--memfd-cache-prime). Synchronously fill, seal and donate
 * every eligible memfd inode in the image, then return so the caller can stop
 * without forking a task tree, making the first real restore a cache hit too.
 * Runs right after prepare_memfd_inodes() in crtools_prepare_shared(), where the
 * image dir and page-read machinery are ready and memfd_shmem_fill_content() is
 * known-good (it is self-contained, no task/pid/ns dependency).
 */
int memfd_content_prime(void)
{
	struct memfd_restore_inode *inode;
	const char *cache_id;
	int cache_sock, primed = 0, warm = 0;

	cache_sock = memfd_cache_sock();
	if (cache_sock < 0 || !opts.memfd_cache_id || !opts.memfd_cache_id[0]) {
		pr_err("memfd-cache prime needs CRIU_MEMFD_CACHE_SOCK and --memfd-cache-id\n");
		return -1;
	}
	cache_id = opts.memfd_cache_id;

	list_for_each_entry(inode, &memfd_inodes, list) {
		MemfdInodeEntry *mie = inode->mie;
		int fd, flags, cfd;

		if (!memfd_cache_eligible(mie))
			continue;

		/* Already warm from an earlier prime/restore: skip the fill. */
		if (memfd_cache_get(cache_sock, mie, cache_id, &cfd) == MEMFD_CACHE_R_HIT) {
			close(cfd);
			warm++;
			continue;
		}

		flags = MFD_ALLOW_SEALING;
		if (mie->has_hugetlb_flag)
			flags |= mie->hugetlb_flag;

		fd = memfd_create(mie->name, flags);
		if (fd < 0) {
			pr_perror("prime: can't create memfd:%s", mie->name);
			return -1;
		}
		if (memfd_shmem_set_size(fd, mie->shmid, mie->size) < 0 ||
		    memfd_shmem_fill_content(fd, mie->shmid, mie->size) < 0) {
			close(fd);
			return -1;
		}
		/*
		 * Apply the dumped seals (incl F_SEAL_FUTURE_WRITE) now: fill has
		 * already munmap'd its writable mapping, so the future-write seal
		 * can be set, producing the same final artifact as apply_memfd_seals.
		 */
		if (mie->seals && fcntl(fd, F_ADD_SEALS, mie->seals) < 0) {
			pr_perror("prime: can't seal memfd:%s", mie->name);
			close(fd);
			return -1;
		}
		if (memfd_cache_donate(cache_sock, fd, mie, cache_id) < 0) {
			close(fd);
			pr_warn("prime: donate failed (socket down); stopping\n");
			break;
		}
		close(fd);
		primed++;
	}

	pr_info("memfd-cache prime: %d donated, %d already warm\n", primed, warm);
	return 0;
}
