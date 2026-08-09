#ifndef __CR_AGENT_CUDA_H__
#define __CR_AGENT_CUDA_H__

#include <stdbool.h>

/*
 * The snapshot agent selects CUDA processes explicitly through
 * CRIU_AGENT_CUDA_VPIDS. CRIU owns the ptrace relationship during restore,
 * so it temporarily runs only each selected process' CUDA restore thread
 * around the agent's CUDA restore RPC notification.
 */
extern int agent_cuda_prepare(void);
extern int agent_cuda_finish(void);
extern bool agent_cuda_requested(void);

#endif /* __CR_AGENT_CUDA_H__ */
