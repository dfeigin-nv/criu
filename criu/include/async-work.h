#ifndef __CR_ASYNC_WORK_H__
#define __CR_ASYNC_WORK_H__

/*
 * Minimal generic async work pool for the restore coordinator.
 *
 * PROTOTYPE (coordinator-hoisted async content restore).
 *
 * Fork-unsafe by design: build the job queue with no threads running
 * (submit), start the workers only after the restore FORKING barrier, and
 * drain (join) them before any subsequent fork(). This guarantees worker
 * threads are never alive across a fork(), so no pthread_atfork() /
 * malloc-lock hazards arise.
 *
 * Usage:
 *	w = async_work_create();
 *	async_work_submit(w, fn, arg);		// repeat; before start, no threads
 *	async_work_start(w, n_workers);		// spawn workers after fork barrier
 *	ret = async_work_drain(w);		// join all, aggregate error
 *	async_work_destroy(w);
 */

struct async_work;

extern struct async_work *async_work_create(void);

/*
 * Queue one job. fn(arg) runs on a worker thread and must return 0 on
 * success or a negative value on failure. Must be called before
 * async_work_start().
 */
extern int async_work_submit(struct async_work *w, int (*fn)(void *), void *arg);

/* Spawn n_workers threads that consume the queued jobs. */
extern int async_work_start(struct async_work *w, int n_workers);

/*
 * Wait for all workers to finish the queue and exit. Returns a negative
 * value if any job returned a negative value, otherwise 0.
 */
extern int async_work_drain(struct async_work *w);

/* Free the pool. Call after drain. */
extern void async_work_destroy(struct async_work *w);

#endif /* __CR_ASYNC_WORK_H__ */
