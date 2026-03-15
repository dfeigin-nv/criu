#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include <lz4.h>

#include "page.h"
#include "util.h"
#include "log.h"
#include "compression.h"
#include "common/xmalloc.h"

#undef LOG_PREFIX
#define LOG_PREFIX "compression: "

#define PARALLEL_DECOMPRESS_MIN_BLOCKS_PER_THREAD 8
#define PARALLEL_DECOMPRESS_MIN_BYTES (1UL << 20)

int compress_data(const char *input_data, size_t input_size,
		  char *compressed_data, size_t output_size,
		  int acceleration)
{
	int ret;

	if (acceleration < 1)
		acceleration = 1;

	ret = LZ4_compress_fast(input_data, compressed_data, input_size,
				output_size, acceleration);
	if (ret <= 0) {
		pr_err("Failed to compress data: %d\n", ret);
		return -1;
	}

	return ret;
}

static int decompress_data_nolog(const char *compressed_data, int compressed_size,
				 int original_size, char *decompressed_data)
{
	int ret;

	ret = LZ4_decompress_safe(compressed_data, decompressed_data,
				  compressed_size, original_size);
	return ret == original_size ? 0 : -1;
}

int decompress_data(const char *compressed_data, int compressed_size,
		    int original_size, char *decompressed_data)
{
	int ret = LZ4_decompress_safe(compressed_data, decompressed_data,
				      compressed_size, original_size);

	if (ret != original_size) {
		pr_err("Decompression failed: expected %d bytes, got %d\n",
		       original_size, ret);
		return -1;
	}

	return 0;
}

/*
 * Region mode compresses @n_pages consecutive pages as one LZ4 block.
 *
 * compress_region() returns the size to store in compressed_size[]:
 * - 0: all-zero region, no payload
 * - n_pages * PAGE_SIZE: raw payload in @dst
 * - otherwise: LZ4 payload in @dst
 */
int compress_region(const char *src, unsigned int n_pages, char *dst,
		    size_t dst_cap, int acceleration)
{
	size_t region_bytes = (size_t)n_pages * PAGE_SIZE;
	unsigned int i;
	int ret;

	if (n_pages == 0 || n_pages > MAX_REGION_PAGES) {
		pr_err("compress_region: invalid n_pages %u\n", n_pages);
		return -1;
	}

	/* Cheap pre-pass: every page in the region zero-filled? */
	for (i = 0; i < n_pages; i++) {
		if (!page_is_all_zero(src + (size_t)i * PAGE_SIZE))
			break;
	}
	if (i == n_pages)
		return 0;

	if (dst_cap < region_bytes) {
		pr_err("compress_region: dst buffer (%zu) smaller than region (%zu)\n",
		       dst_cap, region_bytes);
		return -1;
	}

	if (acceleration < 1)
		acceleration = 1;

	ret = LZ4_compress_fast(src, dst, region_bytes, dst_cap, acceleration);
	if (ret <= 0 || (size_t)ret >= REGION_COMPRESSION_THRESHOLD(region_bytes)) {
		/*
		 * LZ4 can fail when dst_cap is below LZ4 worst-case bound
		 * (which the caller should size correctly), or when the
		 * compressed size hits the threshold and we'd rather store
		 * raw. Either way, fall back to raw.
		 */
		memcpy(dst, src, region_bytes);
		return region_bytes;
	}

	return ret;
}

static int decompress_region_nolog(const char *src, int compressed_size,
				   unsigned int n_pages, char *dst)
{
	size_t region_bytes = (size_t)n_pages * PAGE_SIZE;

	if (n_pages == 0 || n_pages > MAX_REGION_PAGES)
		return -1;

	if (compressed_size == 0) {
		memset(dst, 0, region_bytes);
		return 0;
	}

	if ((size_t)compressed_size == region_bytes) {
		memcpy(dst, src, region_bytes);
		return 0;
	}

	if ((size_t)compressed_size > region_bytes)
		return -1;

	return decompress_data_nolog(src, compressed_size, region_bytes, dst);
}

int decompress_region(const char *src, int compressed_size,
		      unsigned int n_pages, char *dst)
{
	size_t region_bytes = (size_t)n_pages * PAGE_SIZE;

	if (n_pages == 0 || n_pages > MAX_REGION_PAGES) {
		pr_err("decompress_region: invalid n_pages %u\n", n_pages);
		return -1;
	}

	if ((size_t)compressed_size > region_bytes) {
		pr_err("decompress_region: compressed_size %d > region %zu\n",
		       compressed_size, region_bytes);
		return -1;
	}

	if (decompress_region_nolog(src, compressed_size, n_pages, dst)) {
		pr_err("Region decompression failed (compressed_size=%d, n_pages=%u)\n",
		       compressed_size, n_pages);
		return -1;
	}

	return 0;
}

/*
 * Read exactly @size bytes from @fd, handling short reads.
 */
static int read_full(int fd, void *buf, size_t size)
{
	size_t rd = 0;

	while (rd < size) {
		ssize_t ret = read(fd, (char *)buf + rd, size - rd);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_perror("Failed reading from pipe");
			return -1;
		}
		if (ret == 0) {
			pr_err("Unexpected EOF reading from pipe\n");
			return -1;
		}
		rd += ret;
	}
	return 0;
}

static int pread_img_data(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t rd = 0;

	while (rd < count) {
		ssize_t ret = pread(fd, (char *)buf + rd, count - rd, offset + rd);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_perror("Failed reading compressed data");
			return -1;
		}
		if (ret == 0) {
			pr_err("Unexpected EOF reading compressed data\n");
			return -1;
		}
		rd += ret;
	}
	return 0;
}

static int write_pipe_result(int fd, ssize_t value)
{
	size_t done = 0;

	while (done < sizeof(value)) {
		ssize_t ret = write(fd, (char *)&value + done, sizeof(value) - done);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_perror("Failed writing result");
			return -1;
		}
		if (ret == 0) {
			pr_err("Unexpected short write writing result\n");
			return -1;
		}
		done += ret;
	}
	return 0;
}

static int validate_compressed_sizes(uint32_t *compressed_size, int n_pages,
				     uint64_t total_compressed_size)
{
	uint64_t sum = 0;

	for (int i = 0; i < n_pages; i++) {
		if (compressed_size[i] > PAGE_COMPRESSED_SIZE_BOUND) {
			pr_err("Page %d: compressed_size %u exceeds bound\n",
			       i, compressed_size[i]);
			return -1;
		}
		sum += compressed_size[i];
	}
	if (sum != total_compressed_size) {
		pr_err("Compressed size mismatch: sum %" PRIu64 " != total %" PRIu64 "\n",
		       sum, total_compressed_size);
		return -1;
	}
	return 0;
}

static int validate_compressed_sizes_region(uint32_t *compressed_size,
					    uint16_t *block_pages,
					    int n_blocks,
					    int total_pages,
					    uint64_t total_compressed_size,
					    unsigned int region_pages)
{
	uint64_t sum_cs = 0;
	uint64_t sum_pages = 0;
	int i;

	for (i = 0; i < n_blocks; i++) {
		size_t bound;

		if (block_pages[i] == 0 || block_pages[i] > region_pages) {
			pr_err("Block %d: invalid block_pages %u (region=%u)\n",
			       i, block_pages[i], region_pages);
			return -1;
		}
		bound = REGION_COMPRESSED_SIZE_BOUND(block_pages[i]);
		if (compressed_size[i] > bound) {
			pr_err("Block %d: compressed_size %u exceeds bound %zu\n",
			       i, compressed_size[i], bound);
			return -1;
		}
		sum_cs += compressed_size[i];
		sum_pages += block_pages[i];
	}
	if (sum_cs != total_compressed_size) {
		pr_err("Compressed size mismatch: sum %" PRIu64 " != total %" PRIu64 "\n",
		       sum_cs, total_compressed_size);
		return -1;
	}
	if (sum_pages != (uint64_t)total_pages) {
		pr_err("Block pages mismatch: sum %" PRIu64 " != total %d\n",
		       sum_pages, total_pages);
		return -1;
	}
	return 0;
}

struct decompress_job {
	const char *src;
	char *dst;
	uint32_t compressed_size;
	uint16_t pages;
	int block_index;
};

struct decompress_pool {
	pthread_mutex_t lock;
	pthread_cond_t work;
	pthread_cond_t done;
	pthread_t *threads;
	struct decompress_job *jobs;
	int nr_threads;
	int nr_jobs;
	int next_job;
	int pending;
	int failed;
	int failed_block;
	bool initialized;
	bool stop;
};

static int decompress_job_run(struct decompress_job *job)
{
	size_t block_bytes = (size_t)job->pages * PAGE_SIZE;
	uint32_t cs = job->compressed_size;
	if (cs == 0) {
		/* Zero page or region, already zeroed by MADV_DONTNEED. */
	} else if ((size_t)cs == block_bytes) {
		memcpy(job->dst, job->src, block_bytes);
	} else if (job->pages == 1) {
		if (decompress_data_nolog(job->src, cs, PAGE_SIZE, job->dst))
			return -1;
	} else if (decompress_region_nolog(job->src, cs, job->pages, job->dst)) {
		return -1;
	}

	return 0;
}

static void *decompress_worker(void *arg)
{
	struct decompress_pool *pool = arg;

	while (1) {
		struct decompress_job *job;
		int ret;

		pthread_mutex_lock(&pool->lock);
		while (!pool->stop &&
		       (pool->next_job >= pool->nr_jobs || pool->failed))
			pthread_cond_wait(&pool->work, &pool->lock);
		if (pool->stop) {
			pthread_mutex_unlock(&pool->lock);
			break;
		}

		job = &pool->jobs[pool->next_job++];
		pthread_mutex_unlock(&pool->lock);

		ret = decompress_job_run(job);

		pthread_mutex_lock(&pool->lock);
		if (ret && !pool->failed) {
			pool->failed = ret;
			pool->failed_block = job->block_index;
			pool->pending -= pool->nr_jobs - pool->next_job;
			pool->next_job = pool->nr_jobs;
		}
		pool->pending--;
		if (pool->pending == 0)
			pthread_cond_signal(&pool->done);
		pthread_mutex_unlock(&pool->lock);
	}

	return NULL;
}

static int get_decompress_thread_nr(unsigned int requested)
{
	long nr = sysconf(_SC_NPROCESSORS_ONLN);

	if (requested <= 1)
		return 0;
	if (nr < 1)
		nr = 1;
	if (nr > requested)
		nr = requested;

	return nr;
}

static int decompress_pool_init(struct decompress_pool *pool,
				unsigned int requested_threads)
{
	int i, err;

	memset(pool, 0, sizeof(*pool));
	pool->failed_block = -1;
	pool->nr_threads = get_decompress_thread_nr(requested_threads);
	if (pool->nr_threads <= 1)
		return 0;

	pool->threads = xmalloc(pool->nr_threads * sizeof(*pool->threads));
	if (!pool->threads)
		return -1;

	err = pthread_mutex_init(&pool->lock, NULL);
	if (err) {
		pr_err("Failed to initialize decompression lock: %d\n", err);
		goto err_free_threads;
	}
	err = pthread_cond_init(&pool->work, NULL);
	if (err) {
		pr_err("Failed to initialize decompression work condition: %d\n",
		       err);
		goto err_mutex;
	}
	err = pthread_cond_init(&pool->done, NULL);
	if (err) {
		pr_err("Failed to initialize decompression done condition: %d\n",
		       err);
		goto err_work;
	}

	pool->initialized = true;
	for (i = 0; i < pool->nr_threads; i++) {
		err = pthread_create(&pool->threads[i], NULL,
				     decompress_worker, pool);
		if (err) {
			pr_err("Failed to create decompression worker: %d\n",
			       err);
			goto err_threads;
		}
	}

	pr_info("Started %d decompression workers\n", pool->nr_threads);
	return 0;

err_threads:
	pthread_mutex_lock(&pool->lock);
	pool->stop = true;
	pthread_cond_broadcast(&pool->work);
	pthread_mutex_unlock(&pool->lock);
	while (--i >= 0)
		pthread_join(pool->threads[i], NULL);
	pthread_cond_destroy(&pool->done);
err_work:
	pthread_cond_destroy(&pool->work);
err_mutex:
	pthread_mutex_destroy(&pool->lock);
err_free_threads:
	xfree(pool->threads);
	memset(pool, 0, sizeof(*pool));
	return -1;
}

static void decompress_pool_destroy(struct decompress_pool *pool)
{
	int i;

	if (!pool->initialized)
		return;

	pthread_mutex_lock(&pool->lock);
	pool->stop = true;
	pthread_cond_broadcast(&pool->work);
	pthread_mutex_unlock(&pool->lock);

	for (i = 0; i < pool->nr_threads; i++)
		pthread_join(pool->threads[i], NULL);

	pthread_cond_destroy(&pool->done);
	pthread_cond_destroy(&pool->work);
	pthread_mutex_destroy(&pool->lock);
	xfree(pool->threads);
	memset(pool, 0, sizeof(*pool));
}

static int decompress_jobs_parallel(struct decompress_pool *pool,
				    struct decompress_job *jobs, int nr_jobs)
{
	int ret;

	if (nr_jobs == 0)
		return 0;

	pthread_mutex_lock(&pool->lock);
	pool->jobs = jobs;
	pool->nr_jobs = nr_jobs;
	pool->next_job = 0;
	pool->pending = nr_jobs;
	pool->failed = 0;
	pool->failed_block = -1;
	pthread_cond_broadcast(&pool->work);

	while (pool->pending > 0)
		pthread_cond_wait(&pool->done, &pool->lock);

	ret = pool->failed;
	if (ret)
		pr_err("Parallel decompression failed at block %d\n",
		       pool->failed_block);
	pthread_mutex_unlock(&pool->lock);

	return ret ? -1 : 0;
}

static int decompress_jobs_serial(struct decompress_job *jobs, int nr_jobs)
{
	int i;

	for (i = 0; i < nr_jobs; i++) {
		if (decompress_job_run(&jobs[i]))
			return -1;
	}

	return 0;
}

static bool should_decompress_parallel(struct decompress_pool *pool,
				       int nr_jobs, ssize_t total_uncompressed)
{
	return pool->initialized &&
	       nr_jobs >= pool->nr_threads * PARALLEL_DECOMPRESS_MIN_BLOCKS_PER_THREAD &&
	       total_uncompressed >= PARALLEL_DECOMPRESS_MIN_BYTES;
}

static int prepare_decompress_jobs(struct decompress_job *jobs,
				   char *decompressed_buf,
				   char *compressed_buf,
				   const uint32_t *compressed_size,
				   const uint16_t *block_pages,
				   int n_blocks,
				   uint32_t region_pages,
				   int *nr_jobs)
{
	off_t comp_off = 0;
	int page_idx = 0;
	int b;

	*nr_jobs = 0;

	for (b = 0; b < n_blocks; b++) {
		uint16_t pages = region_pages > 0 ? block_pages[b] : 1;
		uint32_t cs = compressed_size[b];

		if (cs != 0) {
			struct decompress_job *job = &jobs[(*nr_jobs)++];

			job->src = compressed_buf + comp_off;
			job->dst = decompressed_buf + (size_t)page_idx * PAGE_SIZE;
			job->compressed_size = cs;
			job->pages = pages;
			job->block_index = b;
		}

		comp_off += cs;
		page_idx += pages;
	}

	return 0;
}

/* Check if addr is right after the end of iov */
static inline bool iov_extends(const struct iovec *iov, const void *addr)
{
	return iov->iov_base + iov->iov_len == addr;
}

/*
 * Build iovec pairs for process_vm_writev().
 *
 * Blocks with compressed_size == 0 have no image payload, but
 * decompressed_buf is reset with MADV_DONTNEED before each batch, so those
 * slots read as zero-filled pages. They must still be written to the remote
 * process: a restored MAP_PRIVATE file mapping can otherwise keep non-zero
 * file contents where the checkpoint image recorded a private zero page.
 *
 * Returns the number of iovecs built and total bytes to write.
 */
static int build_write_iovecs(struct iovec *local_iovs, struct iovec *remote_iovs_wr,
			      struct iovec *remote_iovs, char *decompressed_buf,
			      int nr_iovs, ssize_t *bytes_to_write)
{
	int nio = 0;
	int pi = 0;

	*bytes_to_write = 0;

	for (int i = 0; i < nr_iovs; i++) {
		int iov_pages = remote_iovs[i].iov_len / PAGE_SIZE;
		char *rbase = remote_iovs[i].iov_base;

		for (int j = 0; j < iov_pages; j++, pi++) {
			char *laddr = decompressed_buf + pi * PAGE_SIZE;

			if (nio > 0 && iov_extends(&local_iovs[nio - 1], laddr) &&
				       iov_extends(&remote_iovs_wr[nio - 1], rbase)) {
				local_iovs[nio - 1].iov_len += PAGE_SIZE;
				remote_iovs_wr[nio - 1].iov_len += PAGE_SIZE;
			} else {
				local_iovs[nio].iov_base = laddr;
				local_iovs[nio].iov_len = PAGE_SIZE;
				remote_iovs_wr[nio].iov_base = rbase;
				remote_iovs_wr[nio].iov_len = PAGE_SIZE;
				nio++;
			}
			rbase += PAGE_SIZE;
			*bytes_to_write += PAGE_SIZE;
		}
	}
	return nio;
}

/*
 * Write iovecs to a remote process, handling short writes and
 * the IOV_MAX limit on the number of iovecs per call.
 */
static int vm_writev_all(pid_t pid, struct iovec *local, struct iovec *remote,
			 int nio, ssize_t total)
{
	int iov_off = 0;
	ssize_t written = 0;

	while (written < total) {
		int cnt = nio - iov_off;
		ssize_t ret;

		if (cnt > IOV_MAX)
			cnt = IOV_MAX;

		ret = process_vm_writev(pid, local + iov_off, cnt, remote + iov_off, cnt, 0);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_perror("process_vm_writev failed");
			return -1;
		}
		if (ret == 0) {
			pr_err("process_vm_writev returned 0\n");
			return -1;
		}
		written += ret;

		/* Skip fully written iovecs */
		while (iov_off < nio && ret >= (ssize_t)local[iov_off].iov_len) {
			ret -= local[iov_off].iov_len;
			iov_off++;
		}
		/* Adjust partially written iovec */
		if (ret > 0 && iov_off < nio) {
			local[iov_off].iov_base += ret;
			local[iov_off].iov_len -= ret;
			remote[iov_off].iov_base += ret;
			remote[iov_off].iov_len -= ret;
		}
	}

	return 0;
}

static int vma_io_compress_loop(int pages_img_fd, int sk,
				unsigned int decompress_threads)
{
	struct decompress_pool decompress_pool;
	/* Pre-allocated reusable buffers */
	size_t bp_cap = 0;
	size_t cs_cap = 0;
	size_t jobs_cap = 0;
	size_t iovs_cap = 0;
	size_t comp_cap = 0, decomp_cap = 0;
	uint16_t *block_pages = NULL;
	uint32_t *compressed_size = NULL;
	char *compressed_buf = NULL, *decompressed_buf = NULL;
	struct decompress_job *decompress_jobs = NULL;
	struct iovec *remote_iovs = NULL, *local_iovs = NULL;
	struct iovec *remote_iovs_wr = NULL;  /* write-side remote iovecs */
	int exit_code = -1;

	if (decompress_pool_init(&decompress_pool,
				 decompress_threads))
		pr_warn("Failed to start decompression workers, using serial decompression\n");

	/* Hint the kernel for sequential readahead on the pages image */
	posix_fadvise(pages_img_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

	/*
	 * Protocol (must match compressed_preadv() in restorer):
	 *   1. struct vma_io_compress_hdr (pid, offs, total_cs, n_pages,
	 *                                  nr_iovs, n_blocks, region_pages)
	 *   2. uint32_t compressed_size[n_blocks]
	 *   3. uint16_t block_pages[n_blocks]   (only when region_pages > 0)
	 *   4. struct iovec iovs[nr_iovs]       (remote dest layout)
	 *
	 * Response: ssize_t total_uncompressed_size
	 */
	while (1) {
		struct vma_io_compress_hdr hdr;
		ssize_t total_uncompressed;
		int ret;

		ret = read(sk, &hdr, sizeof(hdr));
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_perror("Failed reading header");
			goto out;
		}
		if (ret == 0)
			break; /* EOF, restorer closed the pipe */

		if ((size_t)ret < sizeof(hdr)) {
			if (read_full(sk,
				      (char *)&hdr + ret,
				      sizeof(hdr) - ret))
				goto out;
		}

		if (hdr.n_pages <= 0 || hdr.nr_iovs <= 0 || hdr.n_blocks <= 0) {
			pr_err("Invalid header: n_pages=%d nr_iovs=%d n_blocks=%d\n",
			       hdr.n_pages, hdr.nr_iovs, hdr.n_blocks);
			goto out;
		}
		/*
		 * There can be at most one block and one iovec per page, so
		 * bound both by n_pages to reject a malformed header before
		 * it drives an oversized allocation/read.
		 */
		if (hdr.n_blocks > hdr.n_pages || hdr.nr_iovs > hdr.n_pages) {
			pr_err("Invalid header: n_blocks=%d nr_iovs=%d > n_pages=%d\n",
			       hdr.n_blocks, hdr.nr_iovs, hdr.n_pages);
			goto out;
		}
		if (hdr.region_pages > MAX_REGION_PAGES) {
			pr_err("Invalid header: region_pages=%u > %d\n",
			       hdr.region_pages, MAX_REGION_PAGES);
			goto out;
		}
		if (hdr.region_pages == 0 && hdr.n_blocks != hdr.n_pages) {
			pr_err("Per-page mode but n_blocks(%d) != n_pages(%d)\n",
			       hdr.n_blocks, hdr.n_pages);
			goto out;
		}

		/* Grow per-block compressed_size array if needed */
		if ((size_t)hdr.n_blocks > cs_cap) {
			cs_cap = hdr.n_blocks;
			compressed_size = xrealloc(compressed_size, cs_cap * sizeof(uint32_t));
			if (!compressed_size)
				goto out;
		}

		if (read_full(sk, compressed_size, hdr.n_blocks * sizeof(uint32_t)))
			goto out;

		if (hdr.region_pages > 0) {
			if ((size_t)hdr.n_blocks > bp_cap) {
				bp_cap = hdr.n_blocks;
				block_pages = xrealloc(block_pages, bp_cap * sizeof(uint16_t));
				if (!block_pages)
					goto out;
			}
			if (read_full(sk, block_pages,
				      hdr.n_blocks * sizeof(uint16_t)))
				goto out;
			if (validate_compressed_sizes_region(compressed_size,
							     block_pages,
							     hdr.n_blocks,
							     hdr.n_pages,
							     hdr.total_compressed_size,
							     hdr.region_pages))
				goto out;
		} else {
			if (validate_compressed_sizes(compressed_size, hdr.n_blocks,
						      hdr.total_compressed_size))
				goto out;
		}
		/*
		 * Grow iovec arrays if needed. The write-side arrays
		 * need room for up to n_pages entries because zero-page
		 * splitting may produce one iovec per non-zero page.
		 */
		{
			size_t need = hdr.n_pages > hdr.nr_iovs ?
				      hdr.n_pages : hdr.nr_iovs;

			if (need > iovs_cap) {
				size_t sz = need * sizeof(struct iovec);

				iovs_cap = need;
				remote_iovs = xrealloc(remote_iovs, sz);
				local_iovs = xrealloc(local_iovs, sz);
				remote_iovs_wr = xrealloc(remote_iovs_wr, sz);
				if (!remote_iovs || !local_iovs || !remote_iovs_wr)
					goto out;
			}
		}

		if (read_full(sk, remote_iovs, hdr.nr_iovs * sizeof(struct iovec)))
			goto out;

		/*
		 * The destination iovecs must describe exactly n_pages whole
		 * pages: build_write_iovecs() derives both the local staging
		 * addresses and the write-side iovec count from them, so a
		 * mismatch would walk past the decompressed buffer and
		 * overflow the write-side iovec arrays (sized for n_pages
		 * entries). Validate them like every other header field.
		 */
		{
			uint64_t iov_bytes = 0;
			int i;

			for (i = 0; i < hdr.nr_iovs; i++) {
				size_t l = remote_iovs[i].iov_len;

				if (l == 0 || l % PAGE_SIZE) {
					pr_err("Invalid iovec length %zu\n", l);
					goto out;
				}
				iov_bytes += l;
			}
			if (iov_bytes != (uint64_t)hdr.n_pages * PAGE_SIZE) {
				pr_err("Iovec bytes %" PRIu64 " do not match n_pages %d\n",
				       iov_bytes, hdr.n_pages);
				goto out;
			}
		}

		/* Grow compressed data buffer if needed */
		if (hdr.total_compressed_size > comp_cap) {
			comp_cap = hdr.total_compressed_size;
			compressed_buf = xrealloc(compressed_buf, comp_cap);
			if (!compressed_buf)
				goto out;
		}

		if (pread_img_data(pages_img_fd, compressed_buf,
				   hdr.total_compressed_size, hdr.offs))
			goto out;
		/*
		 * Release page cache for the data we just read.
		 * This reduces memory pressure during restore of
		 * large processes.
		 */
		posix_fadvise(pages_img_fd, hdr.offs,
			      hdr.total_compressed_size,
			      POSIX_FADV_DONTNEED);

		/*
		 * Grow decompressed buffer if needed. Use mmap for
		 * page alignment. This enables the fast GUP path in
		 * process_vm_writev() and allows THP backing via
		 * MADV_HUGEPAGE to reduce TLB misses.
		 */
		total_uncompressed = (ssize_t)hdr.n_pages * PAGE_SIZE;
		if ((size_t)total_uncompressed > decomp_cap) {
			if (decompressed_buf)
				munmap(decompressed_buf, decomp_cap);
			decomp_cap = total_uncompressed;
			decompressed_buf = mmap(NULL, decomp_cap,
						PROT_READ | PROT_WRITE,
						MAP_PRIVATE | MAP_ANONYMOUS,
						-1, 0);
			if (decompressed_buf == MAP_FAILED) {
				pr_perror("Failed to mmap decompression buffer");
				decompressed_buf = NULL;
				decomp_cap = 0;
				goto out;
			}
			madvise(decompressed_buf, decomp_cap, MADV_HUGEPAGE);
		}

		/* Re-zero buffer so zero-page slots are clean */
		madvise(decompressed_buf, total_uncompressed, MADV_DONTNEED);

		if ((size_t)hdr.n_blocks > jobs_cap) {
			void *new_jobs;

			jobs_cap = hdr.n_blocks;
			new_jobs = xrealloc(decompress_jobs,
					    jobs_cap * sizeof(*decompress_jobs));
			if (!new_jobs)
				goto out;
			decompress_jobs = new_jobs;
		}

		if (hdr.region_pages == 0) {
			int nr_jobs;

			prepare_decompress_jobs(decompress_jobs,
						decompressed_buf,
						compressed_buf,
						compressed_size,
						NULL, hdr.n_blocks,
						hdr.region_pages,
						&nr_jobs);
			if (should_decompress_parallel(&decompress_pool,
						       nr_jobs,
						       total_uncompressed)) {
				if (decompress_jobs_parallel(&decompress_pool,
							     decompress_jobs,
							     nr_jobs))
					goto out;
			} else if (decompress_jobs_serial(decompress_jobs, nr_jobs)) {
				goto out;
			}
		} else {
			/*
			 * Region mode: decompress into the scratch buffer.
			 * Zero regions are already zeroed by MADV_DONTNEED
			 * and are written to the restored process below.
			 */
			{
				int nr_jobs;

				prepare_decompress_jobs(decompress_jobs,
							decompressed_buf,
							compressed_buf,
							compressed_size,
							block_pages,
							hdr.n_blocks,
							hdr.region_pages,
							&nr_jobs);
				if (should_decompress_parallel(&decompress_pool,
							       nr_jobs,
							       total_uncompressed)) {
					if (decompress_jobs_parallel(&decompress_pool,
								     decompress_jobs,
								     nr_jobs))
						goto out;
				} else if (decompress_jobs_serial(decompress_jobs,
								   nr_jobs)) {
					goto out;
				}
			}
		}

		{
			int nio;
			ssize_t bytes_to_write;

			nio = build_write_iovecs(local_iovs, remote_iovs_wr,
						 remote_iovs, decompressed_buf,
						 hdr.nr_iovs, &bytes_to_write);

			if (vm_writev_all(hdr.remote_pid, local_iovs,
					  remote_iovs_wr, nio, bytes_to_write))
				goto out;

			total_uncompressed = bytes_to_write;
		}

		if (write_pipe_result(sk, total_uncompressed))
			goto out;
	}

	exit_code = 0;
out:
	xfree(compressed_size);
	xfree(block_pages);
	xfree(remote_iovs);
	xfree(remote_iovs_wr);
	xfree(local_iovs);
	xfree(decompress_jobs);
	xfree(compressed_buf);
	if (decompressed_buf)
		munmap(decompressed_buf, decomp_cap);
	decompress_pool_destroy(&decompress_pool);
	return exit_code;
}

int start_vma_io_compress_daemon(int pages_img_fd, unsigned int decompress_threads,
				 int *sk, pid_t *pid)
{
	int sks[2] = { -1, -1 };
	pid_t child;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sks)) {
		pr_perror("Failed to create VMA IO socketpair");
		return -1;
	}

	child = fork();
	if (child < 0) {
		pr_perror("Failed to fork VMA IO decompression daemon");
		close(sks[0]);
		close(sks[1]);
		return -1;
	}

	if (child == 0) {
		int ret;

		close(sks[0]);
		ret = vma_io_compress_loop(pages_img_fd, sks[1],
					   decompress_threads);
		close(sks[1]);
		close(pages_img_fd);
		_exit(ret ? 1 : 0);
	}

	close(sks[1]);
	*sk = sks[0];
	*pid = child;
	return 0;
}
