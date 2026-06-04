#ifndef __CR_SHMEM_H__
#define __CR_SHMEM_H__

#include "int.h"
#include "common/lock.h"
#include "images/vma.pb-c.h"
#include "pagemap-cache.h"

struct vma_area;

extern int collect_shmem(int pid, struct vma_area *vma);
extern int collect_sysv_shmem(unsigned long shmid, unsigned long size);
extern int cr_dump_shmem(void);
extern int add_shmem_area(pid_t pid, VmaEntry *vma, pmc_t *pmc);
extern int fixup_sysv_shmems(void);
extern int dump_one_memfd_shmem(int fd, unsigned long shmid, unsigned long size);
extern int dump_one_sysv_shmem(void *addr, unsigned long size, unsigned long shmid);
extern int restore_sysv_shmem_content(void *addr, unsigned long size, unsigned long shmid);
extern int restore_shmem_fd_content(int fd, unsigned long shmid, unsigned long size);

struct async_restore_shmem_args {
	unsigned long shmid;
	unsigned long size;
};

int async_restore_shmem_content(void *arg, int fd, pid_t pid);

/* Split memfd content restore (memfd-cache): set size, then fill/borrow. */
extern int memfd_shmem_set_size(int fd, unsigned long shmid, unsigned long size);
extern int memfd_shmem_fill_content(int fd, unsigned long shmid, unsigned long size);

#define SYSV_SHMEM_SKIP_FD (0x7fffffff)

#endif /* __CR_SHMEM_H__ */
