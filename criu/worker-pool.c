#include <pthread.h>
#include <stdbool.h>

#include "common/list.h"
#include "xmalloc.h"
#include "log.h"
#include "worker-pool.h"

#undef LOG_PREFIX
#define LOG_PREFIX "worker-pool: "

struct work_item {
	struct list_head list;
	int (*fn)(void *);
	void *arg;
};

struct worker_pool {
	struct list_head items;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t *threads;
	int n_threads;
	bool draining;
	int error;
};

struct worker_pool *worker_pool_create(void)
{
	struct worker_pool *wp;

	wp = xzalloc(sizeof(*wp));
	if (!wp)
		return NULL;

	INIT_LIST_HEAD(&wp->items);

	if (pthread_mutex_init(&wp->lock, NULL)) {
		pr_perror("Can't init mutex");
		xfree(wp);
		return NULL;
	}
	if (pthread_cond_init(&wp->cond, NULL)) {
		pr_perror("Can't init cond");
		pthread_mutex_destroy(&wp->lock);
		xfree(wp);
		return NULL;
	}

	return wp;
}

int worker_pool_add(struct worker_pool *wp, int (*fn)(void *), void *arg)
{
	struct work_item *wi;

	wi = xmalloc(sizeof(*wi));
	if (!wi)
		return -1;

	wi->fn = fn;
	wi->arg = arg;

	pthread_mutex_lock(&wp->lock);
	list_add_tail(&wi->list, &wp->items);
	pthread_cond_signal(&wp->cond);
	pthread_mutex_unlock(&wp->lock);

	return 0;
}

static void *pool_worker(void *arg)
{
	struct worker_pool *wp = arg;

	while (1) {
		struct work_item *wi;
		int ret;

		pthread_mutex_lock(&wp->lock);
		while (list_empty(&wp->items) && !wp->draining)
			pthread_cond_wait(&wp->cond, &wp->lock);

		if (list_empty(&wp->items)) {
			/* Draining and nothing left to do. */
			pthread_mutex_unlock(&wp->lock);
			break;
		}

		wi = list_first_entry(&wp->items, struct work_item, list);
		list_del(&wi->list);
		pthread_mutex_unlock(&wp->lock);

		ret = wi->fn(wi->arg);
		xfree(wi);

		if (ret < 0) {
			pthread_mutex_lock(&wp->lock);
			wp->error = -1;
			pthread_mutex_unlock(&wp->lock);
		}
	}

	return NULL;
}

int worker_pool_run(struct worker_pool *wp, int n_workers)
{
	int i;

	if (n_workers < 1)
		n_workers = 1;

	wp->threads = xmalloc(n_workers * sizeof(pthread_t));
	if (!wp->threads)
		return -1;

	for (i = 0; i < n_workers; i++) {
		if (pthread_create(&wp->threads[i], NULL, pool_worker, wp)) {
			pr_perror("Can't create worker thread %d/%d", i, n_workers);
			/*
			 * Join the workers already started so no thread is left
			 * alive on the error path, then report failure. The pool
			 * is still owned by the caller, which must destroy it.
			 */
			wp->n_threads = i;
			worker_pool_wait(wp);
			return -1;
		}
	}

	wp->n_threads = n_workers;
	return 0;
}

int worker_pool_wait(struct worker_pool *wp)
{
	int i;

	pthread_mutex_lock(&wp->lock);
	wp->draining = true;
	pthread_cond_broadcast(&wp->cond);
	pthread_mutex_unlock(&wp->lock);

	for (i = 0; i < wp->n_threads; i++)
		pthread_join(wp->threads[i], NULL);

	wp->n_threads = 0;

	return wp->error;
}

void worker_pool_destroy(struct worker_pool *wp)
{
	struct work_item *wi, *tmp;

	if (!wp)
		return;

	list_for_each_entry_safe(wi, tmp, &wp->items, list) {
		list_del(&wi->list);
		xfree(wi);
	}

	if (wp->threads)
		xfree(wp->threads);

	pthread_cond_destroy(&wp->cond);
	pthread_mutex_destroy(&wp->lock);
	xfree(wp);
}

/*
 * Process-global pool backing the queue/start/join/cleanup facade. Created on
 * the first worker_pool_queue() and torn down by worker_pool_cleanup().
 */
static struct worker_pool *global_worker_pool;

int worker_pool_queue(int (*fn)(void *), void *arg)
{
	if (!global_worker_pool) {
		global_worker_pool = worker_pool_create();
		if (!global_worker_pool)
			return -1;
	}

	return worker_pool_add(global_worker_pool, fn, arg);
}

int worker_pool_start(int n_workers)
{
	if (!global_worker_pool)
		return 0;

	return worker_pool_run(global_worker_pool, n_workers);
}

int worker_pool_join(void)
{
	if (!global_worker_pool)
		return 0;

	return worker_pool_wait(global_worker_pool);
}

void worker_pool_cleanup(void)
{
	worker_pool_destroy(global_worker_pool);
	global_worker_pool = NULL;
}
