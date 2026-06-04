#ifndef __CR_WORKER_POOL_H__
#define __CR_WORKER_POOL_H__

/*
 * Minimal generic worker pool for the restore coordinator.
 *
 * Fork-unsafe by design: build the work-item queue while no worker threads are
 * running (worker_pool_add), start the workers only after the restore FORKING
 * barrier, and wait (join) for them before any subsequent fork(). This keeps
 * worker threads from ever being alive across a fork(), so none of the
 * pthread_atfork() / malloc-lock hazards arise.
 *
 * Object API:
 *	wp = worker_pool_create();
 *	worker_pool_add(wp, fn, arg);	// repeat; queue items, no threads yet
 *	worker_pool_run(wp, n_workers);	// spawn workers after the fork barrier
 *	ret = worker_pool_wait(wp);	// join all, aggregate the error
 *	worker_pool_destroy(wp);
 *
 * fn(arg) runs on a worker thread and must return 0 on success or a negative
 * value on failure; worker_pool_wait() returns a negative value if any item did.
 */

struct worker_pool;

extern struct worker_pool *worker_pool_create(void);

/*
 * Queue one work item. Must be called before worker_pool_run(), while no worker
 * threads are running.
 */
extern int worker_pool_add(struct worker_pool *wp, int (*fn)(void *), void *arg);

/* Spawn n_workers threads that consume the queued work items. */
extern int worker_pool_run(struct worker_pool *wp, int n_workers);

/*
 * Wait for all workers to drain the queue and exit. Returns a negative value
 * if any work item returned a negative value, otherwise 0.
 */
extern int worker_pool_wait(struct worker_pool *wp);

/* Free the pool. Call after worker_pool_wait(). */
extern void worker_pool_destroy(struct worker_pool *wp);

/*
 * Thin facade over a single lazily-created process-global pool, so callers in
 * the restore coordinator can queue work without threading a handle around:
 *
 *	worker_pool_queue(fn, arg);	// pre-fork: create-on-first-use and queue
 *	worker_pool_start(n_workers);	// after the fork barrier
 *	ret = worker_pool_join();	// before the next fork
 *	worker_pool_cleanup();		// idempotent; frees the global pool
 */
extern int worker_pool_queue(int (*fn)(void *), void *arg);
extern int worker_pool_start(int n_workers);
extern int worker_pool_join(void);
extern void worker_pool_cleanup(void);

#endif /* __CR_WORKER_POOL_H__ */
