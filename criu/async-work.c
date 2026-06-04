#include <pthread.h>
#include <stdbool.h>

#include "common/list.h"
#include "xmalloc.h"
#include "log.h"
#include "async-work.h"

#undef LOG_PREFIX
#define LOG_PREFIX "async-work: "

struct async_job {
	struct list_head list;
	int (*fn)(void *);
	void *arg;
};

struct async_work {
	struct list_head jobs;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	pthread_t *threads;
	int n_threads;
	bool draining;
	int error;
};

struct async_work *async_work_create(void)
{
	struct async_work *w;

	w = xzalloc(sizeof(*w));
	if (!w)
		return NULL;

	INIT_LIST_HEAD(&w->jobs);

	if (pthread_mutex_init(&w->lock, NULL)) {
		pr_perror("Can't init mutex");
		xfree(w);
		return NULL;
	}
	if (pthread_cond_init(&w->cond, NULL)) {
		pr_perror("Can't init cond");
		pthread_mutex_destroy(&w->lock);
		xfree(w);
		return NULL;
	}

	return w;
}

int async_work_submit(struct async_work *w, int (*fn)(void *), void *arg)
{
	struct async_job *j;

	j = xmalloc(sizeof(*j));
	if (!j)
		return -1;

	j->fn = fn;
	j->arg = arg;

	pthread_mutex_lock(&w->lock);
	list_add_tail(&j->list, &w->jobs);
	pthread_cond_signal(&w->cond);
	pthread_mutex_unlock(&w->lock);

	return 0;
}

static void *async_worker(void *arg)
{
	struct async_work *w = arg;

	while (1) {
		struct async_job *j;
		int ret;

		pthread_mutex_lock(&w->lock);
		while (list_empty(&w->jobs) && !w->draining)
			pthread_cond_wait(&w->cond, &w->lock);

		if (list_empty(&w->jobs)) {
			/* Draining and nothing left to do. */
			pthread_mutex_unlock(&w->lock);
			break;
		}

		j = list_first_entry(&w->jobs, struct async_job, list);
		list_del(&j->list);
		pthread_mutex_unlock(&w->lock);

		ret = j->fn(j->arg);
		xfree(j);

		if (ret < 0) {
			pthread_mutex_lock(&w->lock);
			w->error = -1;
			pthread_mutex_unlock(&w->lock);
		}
	}

	return NULL;
}

int async_work_start(struct async_work *w, int n_workers)
{
	int i;

	if (n_workers < 1)
		n_workers = 1;

	w->threads = xmalloc(n_workers * sizeof(pthread_t));
	if (!w->threads)
		return -1;

	for (i = 0; i < n_workers; i++) {
		if (pthread_create(&w->threads[i], NULL, async_worker, w)) {
			pr_perror("Can't create worker thread %d/%d", i, n_workers);
			/*
			 * Let the workers we already started drain the queue
			 * and join them, so no thread is left alive on the
			 * error path.
			 */
			w->n_threads = i;
			async_work_drain(w);
			return -1;
		}
	}

	w->n_threads = n_workers;
	return 0;
}

int async_work_drain(struct async_work *w)
{
	int i;

	pthread_mutex_lock(&w->lock);
	w->draining = true;
	pthread_cond_broadcast(&w->cond);
	pthread_mutex_unlock(&w->lock);

	for (i = 0; i < w->n_threads; i++)
		pthread_join(w->threads[i], NULL);

	w->n_threads = 0;

	return w->error;
}

void async_work_destroy(struct async_work *w)
{
	struct async_job *j, *tmp;

	if (!w)
		return;

	list_for_each_entry_safe(j, tmp, &w->jobs, list) {
		list_del(&j->list);
		xfree(j);
	}

	if (w->threads)
		xfree(w->threads);

	pthread_cond_destroy(&w->cond);
	pthread_mutex_destroy(&w->lock);
	xfree(w);
}
