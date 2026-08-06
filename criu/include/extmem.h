#ifndef __CR_EXTMEM_H__
#define __CR_EXTMEM_H__

#include <stdbool.h>
#include <sys/types.h>

int extmem_init(void);
int extmem_open_image(const char *name, int flags, int *fd);
int extmem_get_vma(pid_t pid, unsigned long vaddr, unsigned long length, int *fd);
int extmem_get_shared(unsigned long shmid, unsigned long length, int *fd);
int extmem_validate_memfd(int fd, unsigned long length);
void extmem_report_timings(void);
int extmem_commit(void);
int extmem_abort(void);
bool extmem_enabled(void);

#endif
