#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "zdtmtst.h"

const char *test_doc = "Check memory page content integrity across checkpoint/restore with compression";
const char *test_author = "Radostin Stoyanov <rstoyanov@fedoraproject.org>";

/*
 * Allocate memory regions with different content patterns that
 * exercise each compression code path:
 *
 *   1. Zero-filled pages   (compressed_size=0, no image data)
 *   2. Compressible pages  (LZ4 compressed blocks)
 *   3. Incompressible pages (stored raw, compressed_size == PAGE_SIZE)
 *   4. Private zero pages over a non-zero file mapping
 *
 * After restore, verify every byte matches the original pattern.
 */

#define NR_ZERO_PAGES	100
#define NR_COMP_PAGES	100
#define NR_RAND_PAGES	56
#define NR_FILE_ZERO_PAGES	1
#define TOTAL_PAGES	(NR_ZERO_PAGES + NR_COMP_PAGES + NR_RAND_PAGES)

static uint32_t xorshift32(uint32_t *state)
{
	uint32_t s = *state;

	s ^= s << 13;
	s ^= s >> 17;
	s ^= s << 5;
	*state = s;
	return s;
}

static void fill_pattern(char *buf, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages * (int)PAGE_SIZE; i++)
		buf[i] = i & 0xff;
}

static void fill_random(char *buf, int nr_pages, uint32_t seed)
{
	int i;

	for (i = 0; i < nr_pages * (int)PAGE_SIZE; i++)
		buf[i] = xorshift32(&seed) & 0xff;
}

static int verify_zero(const char *buf, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages * (int)PAGE_SIZE; i++) {
		if (buf[i] != 0) {
			fail("zero region: byte %d is %d, expected 0", i, buf[i]);
			return -1;
		}
	}
	return 0;
}

static int verify_pattern(const char *buf, int nr_pages)
{
	int i;

	for (i = 0; i < nr_pages * (int)PAGE_SIZE; i++) {
		char expected = i & 0xff;

		if (buf[i] != expected) {
			fail("pattern region: byte %d is %d, expected %d",
			     i, buf[i] & 0xff, expected & 0xff);
			return -1;
		}
	}
	return 0;
}

static int verify_random(const char *buf, int nr_pages, uint32_t seed)
{
	int i;

	for (i = 0; i < nr_pages * (int)PAGE_SIZE; i++) {
		char expected = xorshift32(&seed) & 0xff;

		if (buf[i] != expected) {
			fail("random region: byte %d is %d, expected %d",
			     i, buf[i] & 0xff, expected & 0xff);
			return -1;
		}
	}
	return 0;
}

static int setup_file_zero_mapping(char **file_zero_region, char *filename)
{
	char buf[PAGE_SIZE];
	int fd;

	memset(buf, 0x7f, sizeof(buf));

	fd = mkstemp(filename);
	if (fd < 0) {
		pr_perror("mkstemp");
		return -1;
	}

	if (write(fd, buf, sizeof(buf)) != sizeof(buf)) {
		pr_perror("write");
		close(fd);
		unlink(filename);
		return -1;
	}

	*file_zero_region = mmap(NULL, NR_FILE_ZERO_PAGES * PAGE_SIZE,
				 PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
	close(fd);
	if (*file_zero_region == MAP_FAILED) {
		pr_perror("mmap file private");
		unlink(filename);
		return -1;
	}

	/*
	 * Create a private zero page over non-zero file contents. Compressed
	 * restore must write this zero page back into the target process; if it
	 * skips compressed_size == 0 pages, the restored MAP_PRIVATE mapping
	 * falls back to the file's 0x7f bytes instead.
	 */
	memset(*file_zero_region, 0, NR_FILE_ZERO_PAGES * PAGE_SIZE);

	return 0;
}

int main(int argc, char **argv)
{
	char *zero_region, *comp_region, *rand_region, *file_zero_region;
	char file_zero_name[] = "compress_pages00.XXXXXX";
	uint32_t seed = 0xDEADBEEF;
	int ret = 1;

	test_init(argc, argv);

	/*
	 * Use mmap to ensure page-aligned allocations.
	 * MAP_ANONYMOUS pages are zero-filled by the kernel.
	 */
	zero_region = mmap(NULL, NR_ZERO_PAGES * PAGE_SIZE,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	comp_region = mmap(NULL, NR_COMP_PAGES * PAGE_SIZE,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	rand_region = mmap(NULL, NR_RAND_PAGES * PAGE_SIZE,
			   PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (zero_region == MAP_FAILED || comp_region == MAP_FAILED ||
	    rand_region == MAP_FAILED) {
		pr_perror("mmap");
		return 1;
	}

	if (setup_file_zero_mapping(&file_zero_region, file_zero_name))
		return 1;

	/*
	 * Touch every zero page to fault them in. Without this,
	 * the pages might not be present in the pagemap and CRIU
	 * would skip them entirely (not testing compression).
	 */
	memset(zero_region, 0, NR_ZERO_PAGES * PAGE_SIZE);

	/* Fill compressible and random regions */
	fill_pattern(comp_region, NR_COMP_PAGES);
	fill_random(rand_region, NR_RAND_PAGES, seed);

	test_daemon();
	test_waitsig();

	/* After restore: verify every byte */
	if (verify_zero(zero_region, NR_ZERO_PAGES))
		goto err;
	if (verify_pattern(comp_region, NR_COMP_PAGES))
		goto err;
	if (verify_random(rand_region, NR_RAND_PAGES, seed))
		goto err;
	if (verify_zero(file_zero_region, NR_FILE_ZERO_PAGES))
		goto err;

	pass();
	ret = 0;
err:
	unlink(file_zero_name);
	return ret;
}
