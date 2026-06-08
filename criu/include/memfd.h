#ifndef __CR_MEMFD_H__
#define __CR_MEMFD_H__

#include <stdbool.h>
#include <sys/stat.h>

#include "int.h"
#include "common/config.h"

struct fd_parms;
struct file_desc;

extern int is_memfd(dev_t dev);
extern int dump_one_memfd_cond(int lfd, u32 *id, struct fd_parms *parms);
extern const struct fdtype_ops memfd_dump_ops;

extern int memfd_open(struct file_desc *d, u32 *fdflags, bool filemap);
extern struct collect_image_info memfd_cinfo;
extern struct file_desc *collect_memfd(u32 id);
extern int apply_memfd_seals(void);

extern int prepare_memfd_inodes(void);

/*
 * Coordinator-hoisted async memfd content restore. prepare() runs the cheap
 * structural half (create + size + fdstore_add) before fork and queues the
 * content fill; start()/drain() run and join the fill workers after the
 * FORKING barrier; fini() frees the pool on the abort path.
 */
extern int memfd_content_prepare(void);
extern int memfd_content_start(void);
extern int memfd_content_drain(void);
extern void memfd_content_fini(void);

/*
 * Node-local memfd cache donate pass. Runs after apply_memfd_seals(), so the
 * donated fds are final (sealed). For each MISS inode that was eligible at
 * prepare() time, hand the now-sealed populated fd to the cache server. No-op
 * unless the cache is active. Best-effort: failures never fail the restore.
 */
extern int memfd_content_donate(void);

/*
 * Eager cache primer (--memfd-cache-prime): fill + seal + donate every eligible
 * memfd inode in the image, then return so the caller stops before forking a
 * task tree. Makes the first real restore a cache hit too.
 */
extern int memfd_content_prime(void);

#ifdef CONFIG_HAS_MEMFD_CREATE
#include <sys/mman.h>
#else
#include <sys/syscall.h>
#include <linux/memfd.h>
static inline int memfd_create(const char *name, unsigned int flags)
{
	return syscall(SYS_memfd_create, name, flags);
}
#endif /* CONFIG_HAS_MEMFD_CREATE */

#endif /* __CR_MEMFD_H__ */
