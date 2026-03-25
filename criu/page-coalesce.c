#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>

#undef LOG_PREFIX
#define LOG_PREFIX "page-coalesce: "

#include "types.h"
#include "cr_options.h"
#include "image.h"
#include "image-desc.h"
#include "log.h"
#include "pagemap.h"
#include "protobuf.h"
#include "servicefd.h"
#include "xmalloc.h"
#include "images/pagemap.pb-c.h"

#define COALESCE_BATCH_PAGES 16384
#define COALESCE_WORK_PAGES 128
#define COALESCE_HASH_MIN_LOAD 4
#define COALESCE_MAX_THREADS 32
#define COALESCE_INITIAL_SLOTS (1 << 22)

struct page_hash_key {
	u64 w0;
	u64 w1;
	u64 w2;
	u64 w3;
};

struct page_hash_slot {
	struct page_hash_key key;
	u64 offset;
	bool used;
};

struct page_batch_meta {
	struct page_hash_key key;
	bool zero;
};

struct page_target {
	int pagemap_type;
	unsigned long img_id;
};

struct page_store {
	struct page_hash_slot *slots;
	size_t cap;
	size_t grow_at;
	size_t used;
	struct cr_img *blob;
	u64 next_offset;
	u64 grow_us;
	u64 blob_write_us;
};

struct chunk_group_slot {
	struct page_hash_key key;
	unsigned int group_id;
	bool used;
};

struct hash_batch {
	const char *pages;
	struct page_batch_meta *meta;
	unsigned int nr_pages;
	unsigned int generation;
	unsigned int done_workers;
	unsigned int nr_workers;
	unsigned int worker_start[COALESCE_MAX_THREADS];
	unsigned int worker_end[COALESCE_MAX_THREADS];
	bool stop;
	pthread_mutex_t lock;
	pthread_cond_t work_ready;
	pthread_cond_t work_done;
	u64 hash_us;
};

struct hash_worker {
	struct hash_batch *batch;
	unsigned int worker_id;
};

struct hash_pool {
	struct hash_batch batch;
	struct hash_worker *workers;
	pthread_t *threads;
	unsigned int nr_threads;
};

struct coalesce_stats {
	u64 old_bytes;
	u64 blob_bytes;
	u64 index_bytes;
	u64 present_pages;
	u64 zero_pages;
	u64 unique_pages;
	u64 lookup_candidates;
	u64 local_duplicate_pages;
	u64 images;
	u64 total_us;
	u64 read_us;
	u64 hash_us;
	u64 lookup_us;
	u64 blob_write_us;
	u64 index_write_us;
};

static u64 now_us(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return (u64)tv.tv_sec * 1000000ULL + (u64)tv.tv_usec;
}

static inline u64 rotl64(u64 x, int r)
{
	return (x << r) | (x >> (64 - r));
}

static inline u64 fmix64(u64 k)
{
	k ^= k >> 33;
	k *= 0xff51afd7ed558ccdULL;
	k ^= k >> 33;
	k *= 0xc4ceb9fe1a85ec53ULL;
	k ^= k >> 33;
	return k;
}

static inline u64 page_hash_step(u64 state, u64 word, u64 addend, u64 multiplier)
{
	state ^= word + addend;
	state = rotl64(state, 27);
	state *= multiplier;
	return state;
}

static void compute_page_hash_key(const void *page, struct page_hash_key *out, bool *zero)
{
	const u64 *words = page;
	const u64 *end = words + PAGE_SIZE / sizeof(*words);
	u64 h0 = 0x243f6a8885a308d3ULL;
	u64 h1 = 0x13198a2e03707344ULL;
	u64 zero_bits = 0;

	while (words < end) {
		u64 word = *words++;

		zero_bits |= word;
		h0 = page_hash_step(h0, word, 0x9e3779b97f4a7c15ULL, 0xc2b2ae3d27d4eb4fULL);
		h1 = page_hash_step(h1, rotl64(word, 17), 0x94d049bb133111ebULL, 0x165667b19e3779f9ULL);
	}

	*zero = zero_bits == 0;
	h0 = fmix64(h0 ^ PAGE_SIZE);
	h1 = fmix64(h1 ^ (PAGE_SIZE << 1));
	out->w0 = h0;
	out->w1 = h1;
	out->w2 = fmix64(h0 ^ rotl64(h1, 17));
	out->w3 = fmix64(h1 ^ rotl64(h0, 31));
}

static bool page_hash_key_equal(const struct page_hash_key *left, const struct page_hash_key *right)
{
	return left->w0 == right->w0 && left->w1 == right->w1 && left->w2 == right->w2 && left->w3 == right->w3;
}

static size_t page_hash_key_slot(const struct page_hash_key *key)
{
	u64 mixed = key->w0 ^ rotl64(key->w1, 13) ^ rotl64(key->w2, 29) ^ rotl64(key->w3, 47);

	return (size_t)fmix64(mixed);
}

static int init_compat_pagemap_entry(PagemapEntry *pe)
{
	if (pe->has_in_parent && pe->in_parent)
		pe->flags |= PE_PARENT;
	else if (!pe->has_flags)
		pe->flags = PE_PRESENT;

	if (!pe->has_nr_pages)
		pe->nr_pages = pe->compat_nr_pages;

	return 0;
}

static size_t next_power_of_two(size_t value)
{
	size_t power = 1;

	while (power < value)
		power <<= 1;

	return power;
}

static int page_store_resize(struct page_store *store, size_t new_cap)
{
	struct page_hash_slot *new_slots;
	size_t i;
	u64 start_us = now_us();

	new_slots = xzalloc(new_cap * sizeof(*new_slots));
	if (!new_slots)
		return -1;

	for (i = 0; i < store->cap; i++) {
		struct page_hash_slot *slot = &store->slots[i];
		size_t idx;

		if (!slot->used)
			continue;

		idx = page_hash_key_slot(&slot->key) & (new_cap - 1);
		while (new_slots[idx].used)
			idx = (idx + 1) & (new_cap - 1);

		new_slots[idx] = *slot;
	}

	xfree(store->slots);
	store->slots = new_slots;
	store->cap = new_cap;
	store->grow_at = (new_cap * 7) / 10;
	store->grow_us += now_us() - start_us;
	return 0;
}

static int page_store_lookup_or_reserve(struct page_store *store, const struct page_hash_key *key, u64 *offset, bool *is_new)
{
	size_t idx;

	if (!store->cap) {
		if (page_store_resize(store, next_power_of_two(COALESCE_INITIAL_SLOTS)))
			return -1;
	} else if (store->used >= store->grow_at) {
		size_t new_cap = store->cap << 1;

		if (new_cap < store->cap)
			return -1;
		if (page_store_resize(store, new_cap))
			return -1;
	}

	idx = page_hash_key_slot(key) & (store->cap - 1);
	while (store->slots[idx].used) {
		if (page_hash_key_equal(&store->slots[idx].key, key)) {
			*offset = store->slots[idx].offset;
			*is_new = false;
			return 0;
		}
		idx = (idx + 1) & (store->cap - 1);
	}

	store->slots[idx].used = true;
	store->slots[idx].key = *key;
	store->slots[idx].offset = store->next_offset;
	store->used++;

	*offset = store->next_offset;
	store->next_offset += PAGE_SIZE;
	*is_new = true;
	return 0;
}

static int chunk_group_lookup_or_reserve(struct chunk_group_slot *slots, size_t cap, const struct page_hash_key *key,
					 unsigned int new_group_id, unsigned int *group_id, bool *is_new)
{
	size_t idx;

	idx = page_hash_key_slot(key) & (cap - 1);
	while (slots[idx].used) {
		if (page_hash_key_equal(&slots[idx].key, key)) {
			*group_id = slots[idx].group_id;
			*is_new = false;
			return 0;
		}
		idx = (idx + 1) & (cap - 1);
	}

	slots[idx].used = true;
	slots[idx].key = *key;
	slots[idx].group_id = new_group_id;
	*group_id = new_group_id;
	*is_new = true;
	return 0;
}

static void *hash_worker_main(void *arg)
{
	struct hash_worker *worker = arg;
	struct hash_batch *batch = worker->batch;
	unsigned int generation = 0;
	unsigned int worker_id = worker->worker_id;

	for (;;) {
		u64 local_hash_us = 0;
		unsigned int start;
		unsigned int end;
		u64 start_us;

		pthread_mutex_lock(&batch->lock);
		while (!batch->stop && batch->generation == generation)
			pthread_cond_wait(&batch->work_ready, &batch->lock);

		if (batch->stop) {
			pthread_mutex_unlock(&batch->lock);
			break;
		}

		generation = batch->generation;
		start = batch->worker_start[worker_id];
		end = batch->worker_end[worker_id];
		pthread_mutex_unlock(&batch->lock);

		start_us = now_us();
		while (start < end) {
			const char *page = batch->pages + start * PAGE_SIZE;

			compute_page_hash_key(page, &batch->meta[start].key, &batch->meta[start].zero);
			start++;
		}
		local_hash_us += now_us() - start_us;

		pthread_mutex_lock(&batch->lock);
		batch->hash_us += local_hash_us;
		batch->done_workers++;
		if (batch->done_workers == batch->nr_workers)
			pthread_cond_signal(&batch->work_done);
		pthread_mutex_unlock(&batch->lock);
	}

	return NULL;
}

static int hash_pool_init(struct hash_pool *pool)
{
	long cpus;
	unsigned int i;

	memzero(pool, sizeof(*pool));
	cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (cpus < 1)
		cpus = 1;
	pool->nr_threads = (unsigned int)cpus;
	if (pool->nr_threads > COALESCE_MAX_THREADS)
		pool->nr_threads = COALESCE_MAX_THREADS;
	if (pool->nr_threads < 2)
		return 0;

	pool->workers = xmalloc(pool->nr_threads * sizeof(*pool->workers));
	pool->threads = xmalloc(pool->nr_threads * sizeof(*pool->threads));
	if (!pool->workers || !pool->threads)
		return -1;

	pthread_mutex_init(&pool->batch.lock, NULL);
	pthread_cond_init(&pool->batch.work_ready, NULL);
	pthread_cond_init(&pool->batch.work_done, NULL);
	pool->batch.nr_workers = pool->nr_threads;

	for (i = 0; i < pool->nr_threads; i++) {
		pool->workers[i].batch = &pool->batch;
		pool->workers[i].worker_id = i;
		if (pthread_create(&pool->threads[i], NULL, hash_worker_main, &pool->workers[i]) != 0) {
			unsigned int j;

			pool->batch.stop = true;
			pthread_cond_broadcast(&pool->batch.work_ready);
			for (j = 0; j < i; j++)
				pthread_join(pool->threads[j], NULL);
			pthread_cond_destroy(&pool->batch.work_done);
			pthread_cond_destroy(&pool->batch.work_ready);
			pthread_mutex_destroy(&pool->batch.lock);
			xfree(pool->workers);
			xfree(pool->threads);
			memzero(pool, sizeof(*pool));
			pr_err("pthread_create failed for page hash worker\n");
			return -1;
		}
	}

	return 0;
}

static void hash_pool_fini(struct hash_pool *pool)
{
	unsigned int i;

	if (!pool->threads)
		return;

	pthread_mutex_lock(&pool->batch.lock);
	pool->batch.stop = true;
	pthread_cond_broadcast(&pool->batch.work_ready);
	pthread_mutex_unlock(&pool->batch.lock);

	for (i = 0; i < pool->nr_threads; i++)
		pthread_join(pool->threads[i], NULL);

	pthread_cond_destroy(&pool->batch.work_done);
	pthread_cond_destroy(&pool->batch.work_ready);
	pthread_mutex_destroy(&pool->batch.lock);
	xfree(pool->workers);
	xfree(pool->threads);
	memzero(pool, sizeof(*pool));
}

static void hash_pages_serial(const char *pages, struct page_batch_meta *meta, unsigned int nr_pages, u64 *hash_us)
{
	unsigned int i;
	u64 start_us = now_us();

	for (i = 0; i < nr_pages; i++) {
		const char *page = pages + i * PAGE_SIZE;

		compute_page_hash_key(page, &meta[i].key, &meta[i].zero);
	}

	*hash_us += now_us() - start_us;
}

static int hash_pool_run(struct hash_pool *pool, const char *pages, struct page_batch_meta *meta, unsigned int nr_pages,
			 u64 *hash_us)
{
	unsigned int i;

	if (!pool->threads || nr_pages < COALESCE_HASH_MIN_LOAD * COALESCE_WORK_PAGES) {
		hash_pages_serial(pages, meta, nr_pages, hash_us);
		return 0;
	}

	pthread_mutex_lock(&pool->batch.lock);
	pool->batch.pages = pages;
	pool->batch.meta = meta;
	pool->batch.nr_pages = nr_pages;
	pool->batch.done_workers = 0;
	pool->batch.hash_us = 0;
	for (i = 0; i < pool->nr_threads; i++) {
		unsigned int base = nr_pages / pool->nr_threads;
		unsigned int extra = nr_pages % pool->nr_threads;
		unsigned int start = i * base + (i < extra ? i : extra);
		unsigned int span = base + (i < extra ? 1 : 0);

		pool->batch.worker_start[i] = start;
		pool->batch.worker_end[i] = start + span;
	}
	pool->batch.generation++;
	pthread_cond_broadcast(&pool->batch.work_ready);
	while (pool->batch.done_workers != pool->batch.nr_workers)
		pthread_cond_wait(&pool->batch.work_done, &pool->batch.lock);
	*hash_us += pool->batch.hash_us;
	pthread_mutex_unlock(&pool->batch.lock);
	return 0;
}

static int collect_page_targets(int dfd, struct page_target **targets, size_t *nr_targets)
{
	DIR *dir;
	struct dirent *ent;
	size_t cap = 0;
	int dupfd;

	*targets = NULL;
	*nr_targets = 0;

	dupfd = dup(dfd);
	if (dupfd < 0) {
		pr_perror("Can't dup image directory fd");
		return -1;
	}

	dir = fdopendir(dupfd);
	if (!dir) {
		pr_perror("Can't open image directory");
		close(dupfd);
		return -1;
	}

	while ((ent = readdir(dir)) != NULL) {
		struct page_target target;
		int matched = sscanf(ent->d_name, "pagemap-%lu.img", &target.img_id);

		target.pagemap_type = CR_FD_PAGEMAP;
		if (matched != 1) {
			matched = sscanf(ent->d_name, "pagemap-shmem-%lu.img", &target.img_id);
			target.pagemap_type = CR_FD_SHMEM_PAGEMAP;
		}
		if (matched != 1)
			continue;

		if (*nr_targets == cap) {
			size_t new_cap = cap ? cap * 2 : 64;
			struct page_target *grown = xrealloc(*targets, new_cap * sizeof(**targets));
			if (!grown) {
				closedir(dir);
				return -1;
			}
			*targets = grown;
			cap = new_cap;
		}

		(*targets)[*nr_targets] = target;
		(*nr_targets)++;
	}

	if (closedir(dir)) {
		pr_perror("Can't close image directory");
		return -1;
	}

	return 0;
}

static int coalesce_one_pagemap(int dfd, struct hash_pool *pool, struct page_store *store, const struct page_target *target,
				struct coalesce_stats *stats)
{
	char pages_path[PATH_MAX] = {};
	struct page_batch_meta *meta = NULL;
	struct chunk_group_slot *chunk_groups = NULL;
	unsigned int *chunk_group_of_page = NULL;
	unsigned int *chunk_rep_page = NULL;
	u64 *chunk_group_offsets = NULL;
	u64 *offsets = NULL;
	struct iovec *blob_iov = NULL;
	struct cr_img *index = NULL;
	struct cr_img *old_pages = NULL;
	struct cr_img *pagemap = NULL;
	PagemapEntry *pe = NULL;
	u32 pages_id = 0;
	off_t old_pages_size;
	off_t source_off = 0;
	int old_pages_fd;
	const char *old_pages_map = NULL;
	bool old_pages_mapped = false;
	size_t chunk_groups_cap;
	u64 step_start_us = 0;
	u64 image_present_pages = 0;
	u64 image_zero_pages = 0;
	u64 image_unique_pages = 0;
	u64 image_lookup_candidates = 0;
	u64 image_local_duplicate_pages = 0;
	u64 image_total_us = 0;
	u64 image_read_us = 0;
	u64 image_hash_us = 0;
	u64 image_lookup_us = 0;
	u64 image_blob_write_us = 0;
	u64 image_index_write_us = 0;
	int ret = -1;

	meta = xmalloc(COALESCE_BATCH_PAGES * sizeof(*meta));
	offsets = xmalloc(COALESCE_BATCH_PAGES * sizeof(*offsets));
	blob_iov = xmalloc(COALESCE_BATCH_PAGES * sizeof(*blob_iov));
	chunk_group_of_page = xmalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_group_of_page));
	chunk_rep_page = xmalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_rep_page));
	chunk_group_offsets = xmalloc(COALESCE_BATCH_PAGES * sizeof(*chunk_group_offsets));
	chunk_groups_cap = next_power_of_two(COALESCE_BATCH_PAGES * 2);
	chunk_groups = xmalloc(chunk_groups_cap * sizeof(*chunk_groups));
	if (!meta || !offsets || !blob_iov || !chunk_group_of_page || !chunk_rep_page || !chunk_group_offsets || !chunk_groups)
		goto out;

	pagemap = open_image_at(dfd, target->pagemap_type, O_RSTR, target->img_id);
	if (!pagemap || empty_image(pagemap)) {
		pr_err("Missing pagemap image for id %lu\n", target->img_id);
		goto out;
	}

	old_pages = open_pages_image_at(dfd, O_RSTR, pagemap, &pages_id);
	if (!old_pages || empty_image(old_pages)) {
		pr_err("Missing pages image for id %u\n", pages_id);
		goto out;
	}

	index = open_image_at(dfd, CR_FD_PAGE_INDEX, O_DUMP, pages_id);
	if (!index)
		goto out;

	old_pages_fd = img_raw_fd(old_pages);
	old_pages_size = img_raw_size(old_pages);
	if (old_pages_fd < 0 || old_pages_size < 0)
		goto out;

	step_start_us = now_us();
	/* Read the source pages directly from the mapped image to avoid a copy. */
	old_pages_map = mmap(NULL, (size_t)old_pages_size, PROT_READ, MAP_PRIVATE, old_pages_fd, 0);
	if (old_pages_map == MAP_FAILED) {
		pr_perror("Can't mmap pages image for id %u", pages_id);
		old_pages_map = NULL;
		goto out;
	}
	old_pages_mapped = true;
	(void)posix_fadvise(old_pages_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	(void)madvise((void *)old_pages_map, (size_t)old_pages_size, MADV_SEQUENTIAL);
	image_read_us += now_us() - step_start_us;

	while (1) {
		int pb_ret = pb_read_one_eof(pagemap, &pe, PB_PAGEMAP);
		u64 i;

		if (pb_ret < 0)
			goto out;
		if (pb_ret == 0)
			break;

		init_compat_pagemap_entry(pe);
		if (!pagemap_present(pe)) {
			pagemap_entry__free_unpacked(pe, NULL);
			pe = NULL;
			continue;
		}

		for (i = 0; i < pe->nr_pages;) {
			unsigned int chunk_pages = pe->nr_pages - i;
			size_t chunk_bytes;
			u64 step_start_us;
			unsigned int j;
			unsigned int nr_blob_pages = 0;
			u64 lookup_start_us;
			unsigned int group_count = 0;
			unsigned int chunk_nonzero_pages = 0;
			const char *chunk_pages_ptr = old_pages_map + source_off;

			if (chunk_pages > COALESCE_BATCH_PAGES)
				chunk_pages = COALESCE_BATCH_PAGES;
			chunk_bytes = (size_t)chunk_pages * PAGE_SIZE;

			if (hash_pool_run(pool, chunk_pages_ptr, meta, chunk_pages, &image_hash_us))
				goto out;

			/* Collapse identical pages inside the chunk before hitting the global table. */
			memset(chunk_groups, 0, chunk_groups_cap * sizeof(*chunk_groups));
			memset(chunk_group_of_page, 0xff, chunk_pages * sizeof(*chunk_group_of_page));

			for (j = 0; j < chunk_pages; j++) {
				unsigned int group_id;
				bool is_new;

				if (meta[j].zero) {
					offsets[j] = PAGE_INDEX_ZERO;
					image_zero_pages++;
					continue;
				}

				chunk_nonzero_pages++;
				if (chunk_group_lookup_or_reserve(chunk_groups, chunk_groups_cap, &meta[j].key, group_count, &group_id,
								  &is_new))
					goto out;
				chunk_group_of_page[j] = group_id;
				if (is_new)
					chunk_rep_page[group_count++] = j;
			}

			lookup_start_us = now_us();
			for (j = 0; j < group_count; j++) {
				unsigned int rep = chunk_rep_page[j];
				bool is_new;

				if (page_store_lookup_or_reserve(store, &meta[rep].key, &chunk_group_offsets[j], &is_new))
					goto out;
				if (is_new) {
					blob_iov[nr_blob_pages].iov_base = (void *)(chunk_pages_ptr + rep * PAGE_SIZE);
					blob_iov[nr_blob_pages].iov_len = PAGE_SIZE;
					nr_blob_pages++;
					image_unique_pages++;
				}
			}
			image_lookup_us += now_us() - lookup_start_us;
			image_lookup_candidates += group_count;
			image_local_duplicate_pages += chunk_nonzero_pages - group_count;

			for (j = 0; j < chunk_pages; j++) {
				if (!meta[j].zero)
					offsets[j] = chunk_group_offsets[chunk_group_of_page[j]];
			}

			if (nr_blob_pages > 0) {
				step_start_us = now_us();
				for (j = 0; j < nr_blob_pages;) {
					unsigned int batch_iov = nr_blob_pages - j;
					int written;

					if (batch_iov > IOV_MAX)
						batch_iov = IOV_MAX;

					written = bwritev(&store->blob->_x, &blob_iov[j], batch_iov);
					if (written < 0) {
						pr_perror("Can't write coalesced page blob");
						goto out;
					}
					if (written != (int)(batch_iov * PAGE_SIZE)) {
						pr_err("Short write to coalesced page blob: %d/%zu bytes\n", written,
						       (size_t)batch_iov * PAGE_SIZE);
						goto out;
					}
					j += batch_iov;
				}
				store->blob_write_us += now_us() - step_start_us;
			}

			step_start_us = now_us();
			if (write_img_buf(index, offsets, chunk_pages * sizeof(*offsets)))
				goto out;
			image_index_write_us += now_us() - step_start_us;

			image_present_pages += chunk_pages;
			source_off += chunk_bytes;
			i += chunk_pages;
		}

		pagemap_entry__free_unpacked(pe, NULL);
		pe = NULL;
	}

	if (source_off != old_pages_size) {
		pr_err("Pages image size mismatch for id %u: consumed %jd bytes, file has %jd bytes\n", pages_id,
		       (intmax_t)source_off, (intmax_t)old_pages_size);
		goto out;
	}

	snprintf(pages_path, sizeof(pages_path), imgset_template[CR_FD_PAGES].fmt, pages_id);

	image_blob_write_us = store->blob_write_us - stats->blob_write_us;
	image_total_us = image_read_us + image_hash_us + image_lookup_us + image_blob_write_us + image_index_write_us;

	stats->old_bytes += old_pages_size;
	stats->index_bytes += image_present_pages * sizeof(*offsets);
	stats->present_pages += image_present_pages;
	stats->zero_pages += image_zero_pages;
	stats->unique_pages += image_unique_pages;
	stats->lookup_candidates += image_lookup_candidates;
	stats->local_duplicate_pages += image_local_duplicate_pages;
	stats->images++;
	stats->total_us += image_total_us;
	stats->read_us += image_read_us;
	stats->hash_us += image_hash_us;
	stats->lookup_us += image_lookup_us;
	stats->blob_write_us = store->blob_write_us;
	stats->index_write_us += image_index_write_us;

	pr_info("Image %u coalesced: present=%llu unique=%llu local_unique=%llu local_dup=%llu zero=%llu total=%llu ms read=%llu ms hash=%llu ms lookup=%llu ms blob=%llu ms index=%llu ms\n",
		pages_id, (unsigned long long)image_present_pages, (unsigned long long)image_unique_pages,
		(unsigned long long)image_lookup_candidates, (unsigned long long)image_local_duplicate_pages,
		(unsigned long long)image_zero_pages, (unsigned long long)(image_total_us / 1000ULL),
		(unsigned long long)(image_read_us / 1000ULL), (unsigned long long)(image_hash_us / 1000ULL),
		(unsigned long long)(image_lookup_us / 1000ULL), (unsigned long long)(image_blob_write_us / 1000ULL),
		(unsigned long long)(image_index_write_us / 1000ULL));

	ret = 0;
out:
	if (pe)
		pagemap_entry__free_unpacked(pe, NULL);
	if (old_pages_mapped)
		munmap((void *)old_pages_map, old_pages_size);
	if (index)
		close_image(index);
	if (old_pages)
		close_image(old_pages);
	if (pagemap)
		close_image(pagemap);
	xfree(offsets);
	xfree(meta);
	xfree(blob_iov);
	xfree(chunk_group_of_page);
	xfree(chunk_rep_page);
	xfree(chunk_group_offsets);
	xfree(chunk_groups);

	if (ret == 0 && unlinkat(dfd, pages_path, 0)) {
		pr_perror("Can't remove original pages image %s", pages_path);
		ret = -1;
	}

	return ret;
}

int coalesce_checkpoint_pages(void)
{
	struct hash_pool pool;
	struct page_store store = {};
	struct coalesce_stats stats = {};
	struct page_target *targets = NULL;
	size_t i;
	size_t nr_targets = 0;
	int dfd;
	int ret = 0;
	u64 start_us;

	if (!opts.auto_dedup)
		return 0;

	if (opts.lazy_pages || opts.use_page_server || opts.stream) {
		pr_warn("Skipping page coalescing because lazy-pages, page-server, or image streaming is enabled\n");
		return 0;
	}

	dfd = get_service_fd(IMG_FD_OFF);
	if (dfd < 0) {
		pr_err("Image directory is not open\n");
		return -1;
	}

	if (collect_page_targets(dfd, &targets, &nr_targets))
		return -1;
	if (!nr_targets) {
		xfree(targets);
		return 0;
	}

	if (hash_pool_init(&pool)) {
		xfree(targets);
		return -1;
	}

	store.blob = open_image_at(dfd, CR_FD_PAGES_BLOB, O_RDWR | O_CREAT | O_TRUNC);
	if (!store.blob) {
		hash_pool_fini(&pool);
		xfree(targets);
		return -1;
	}

	start_us = now_us();
	for (i = 0; i < nr_targets; i++) {
		if (coalesce_one_pagemap(dfd, &pool, &store, &targets[i], &stats)) {
			ret = -1;
			break;
		}
	}
	stats.total_us = now_us() - start_us;
	stats.blob_write_us = store.blob_write_us;
	stats.blob_bytes = store.next_offset;

	close_image(store.blob);
	hash_pool_fini(&pool);
	xfree(store.slots);
	xfree(targets);

	if (ret)
		return ret;

	pr_info("Coalesced %llu present pages across %llu pagemap images into %llu unique non-zero pages, eliding %llu zero pages\n",
		(unsigned long long)stats.present_pages, (unsigned long long)stats.images,
		(unsigned long long)stats.unique_pages, (unsigned long long)stats.zero_pages);
	pr_info("Chunk-local dedupe candidates: lookup=%llu local_duplicate=%llu\n",
		(unsigned long long)stats.lookup_candidates, (unsigned long long)stats.local_duplicate_pages);
	pr_info("Page payload bytes: old=%llu blob=%llu index=%llu combined=%llu saved=%lld\n",
		(unsigned long long)stats.old_bytes, (unsigned long long)stats.blob_bytes,
		(unsigned long long)stats.index_bytes,
		(unsigned long long)(stats.blob_bytes + stats.index_bytes),
		(long long)(stats.old_bytes - (stats.blob_bytes + stats.index_bytes)));
	pr_info("Coalesce timings: total=%llu ms read=%llu ms hash=%llu ms lookup=%llu ms blob=%llu ms index=%llu ms grow=%llu ms table_used=%zu table_cap=%zu table_load=%llu%% workers=%u batch_pages=%u\n",
		(unsigned long long)(stats.total_us / 1000ULL), (unsigned long long)(stats.read_us / 1000ULL),
		(unsigned long long)(stats.hash_us / 1000ULL), (unsigned long long)(stats.lookup_us / 1000ULL),
		(unsigned long long)(stats.blob_write_us / 1000ULL), (unsigned long long)(stats.index_write_us / 1000ULL),
		(unsigned long long)(store.grow_us / 1000ULL), store.used, store.cap,
		(unsigned long long)(store.cap ? (store.used * 100ULL / store.cap) : 0), pool.nr_threads, COALESCE_BATCH_PAGES);
	return 0;
}
