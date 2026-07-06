# CRIU (Checkpoint/Restore In User-space)

CRIU is a tool for saving the state of a running application to a set of files
(checkpointing) and restoring it back to a live state. It is primarily used for
live migration of containers, in-place updates, and fast application startup.

It is implemented as a command-line tool called `criu`. The two primary commands
are `dump` and `restore`.

- `dump`: Saves a process tree and all its related resources (file
  descriptors, IPC, sockets, namespaces, etc.) into a collection of image
  files.
- `restore`: Restores processes from image files to the same state they were
  in before the dump.

## Quick Start

To get a feel for `criu`, you can try checkpointing and restoring a simple
process.

1.  **Run a simple process:**
    Open a terminal and run a command that will run for a while. Find its PID.
    ```bash
    sleep 1000 &
    [1] 12345
    ```

2.  **Dump the process:**
    As root, use `criu dump` with the process ID (`-t`) and a directory for the
    image files (`-D`).
    ```bash
    sudo criu dump -t 12345 -D /tmp/sleep_images -v4 --shell-job
    ```
    The `sleep` process will no longer be running.

3.  **Restore the process:**
    Use `criu restore` to bring the process back to life from the images.
    ```bash
    sudo criu restore -D /tmp/sleep_images -v4 --shell-job
    ```
    The `sleep` process will be running again as if nothing happened.

# For Developers and Contributors

This section contains more technical details about CRIU's internals and
development process.

## Dump Process

On dump, CRIU uses available kernel interfaces to collect information about
processes. For properties that can only be retrieved from within the process
itself, CRIU injects a binary blob (called a "parasite") into the process's
address space and executes it in the context of one of the process's threads.
This injection is handled by a subproject called **Compel**.

## Restore Process

On restore, CRIU reads the image files to reconstruct the processes. The goal is
to restore them to the exact state they were in before the dump. The restore
process is divided into several stages (defined as `CR_STATE_*` in
`./criu/include/restorer.h`).

The main `criu` process acts as a coordinator. It first restores resources with
inter-process dependencies (file descriptors, sockets, shared memory,
namespaces, etc.). It then forks the process tree and sets up namespaces.
Finally, it restores process-specific resources like file descriptors and memory
mappings.

A key step involves a small, self-contained binary called the "restorer". All
restored processes switch to executing this code, which unmaps the CRIU-specific
memory and restores the application's original memory mappings. On the final
step, the restorer calls `sigreturn` on a prepared signal frame to resume the
process with the state it had at the moment of the dump.

## Compel

Compel is a subproject responsible for generating the binary blobs used for the
parasite code (for dumping) and the restorer code (for restoring). It provides a
library for injecting and executing this code within the target process's
address space. It is a separate project because the logic for generating and
injecting Position-Independent Executable (PIE) code is complex and
self-contained.

## Coding Style

The C code in the CRIU project follows the
[Linux Kernel Coding Style](https://www.kernel.org/doc/html/latest/process/coding-style.html).
Here are some of the main points:

-   **Indentation**: Use tabs, which are set to 8 characters.
-   **Line Length**: The preferred line limit is 80 characters, but it can be
    extended to 120 if it improves code readability.
-   **Braces**:
    -   The opening brace for a function goes on a new line.
    -   The opening brace for a block (like `if`, `for`, `while`, `switch`) goes
        on the same line.
-   **Spaces**: Use spaces around operators (`+`, `-`, `*`, `/`, `%`, `<`, `>`,
    `=`, etc.).
-   **Naming**: Use descriptive names for functions and variables.
-   **Comments**: Use C-style comments (`/* ... */`). For multi-line comments,
    the preferred format is:
    ```c
    /*
     * This is a multi-line
     * comment.
     */
    ```

## Code Layout

The code is organized into the following directories:

-   `./compel`: The Compel sub-project.
-   `./criu`: The main `criu` tool source code.
-   `./images`: Protobuf descriptions for the image files.
-   `./test`: All tests.
-   `./test/zdtm`: The Zero-Downtime Migration (ZDTM) test suite.
-   `./test/zdtm.py`: The executor script for ZDTM tests.
-   `./scripts`: Helper scripts.
-   `./scripts/build`: Docker image files used for CI and cross-compilation
    checks.
-   `./crit`: A tool to inspect and manipulate CRIU image files.
-   `./soccr`: A library for TCP socket checkpoint/restore.

## Tests

The main test suite is ZDTM. Here is an example of how to run a single test:

```bash
sudo ./test/zdtm.py run -t zdtm/static/env00
```

Each ZDTM test has three stages: preparation, C/R, and results checks. During
the test, a process calls `test_daemon()` to signal it is ready for C/R, then
calls `test_waitsig()` to wait for the C/R stage to complete. After being
restored, the test checks that all its resources are still in a valid state.

Build and test in Docker:
```bash
~/Work/checkpoints/criu-test.sh
```

---

# Branch: `add-aio-and-parallel-memfd`

This branch adds three performance improvements to restore throughput for
large GPU workloads (vLLM / CUDA). It is developed on top of CRIU v4.2.

## What It Does

### 1. Parallel memfd open (`criu/mem.c`)

`open_vmas` was rewritten from a sequential `filemap_ctx`-based loop into a
4-phase pipeline:

- **Phase 1**: Walk VMAs; call `vm_open` for shmem/socket VMAs immediately;
  collect file-backed VMAs into `unique[]` (dedup by `(vmfd, flags)`)
- **Phase 2**: `open_vmas_unique_open` — run `memfd_open_parallel` (pthread
  worker pool, up to 8 workers) to open all unique memfd inodes concurrently,
  then open remaining regular-file VMAs sequentially
- **Phase 3**: Assign `vma->e->fd` from `unique[]`
- **Phase 4**: Walk VMAs in order, set `VMA_CLOSE` on the last VMA per fd

`memfd_open` is expensive: it calls `memfd_create + ftruncate +
restore_memfd_shmem_content` (reads the full page data from the checkpoint
image). For vLLM with ~122 memfd regions, this was the main bottleneck.

**Key structs**: `open_vma_unique` (one entry per unique vmfd+flags),
`memfd_open_plan` (shared worker queue).

**Inode dedup**: `memfd_open_jobs_collect` deduplicates by inode (not just
vmfd pointer) so the same physical memfd opened under two different fds with
different flags only gets opened once.

**Thread safety**: Workers write `job->fd` without holding the plan lock
(safe — each job is claimed exclusively via `next_job++`). The `inode->lock`
mutex in `memfd_open_inode` serializes concurrent opens of the same inode.

### 2. Async shmem restore (`criu/shmem.c`, `criu/pagemap.c`)

`do_restore_shmem_content` was refactored into `shmem_restore_async` which
reads all shmem regions with `PR_ASYNC`, then flushes via `pr->sync()`.
`process_async_reads` pre-warms the page cache with `posix_fadvise(WILLNEED)`
before issuing `preadv` calls.

`bfd.c` buffer pool now has a `pthread_mutex_t bufs_lock` for thread safety
since multiple workers call `bfdopenr`/`bfdclose` concurrently.

### 3. Linux AIO in PIE restorer (`criu/pie/restorer.c`)

The sequential `preadv` loop for private VMA pages was replaced with Linux
native AIO (`io_setup` / `io_submit` / `io_getevents`). O_DIRECT is enabled
on the pages fd for higher throughput on NVMe.

**Why in PIE code**: Private VMA pages can only be written after the process
virtual address space is set up via `mmap(MAP_FIXED)`. That happens inside
the PIE restorer (a stripped binary — no libc, raw syscalls only) AFTER CRIU
unmaps itself. So page reads must happen there too.

**Sliding window**: keeps up to `AIO_BATCH=128` in-flight operations. Short
reads (kernel `MAX_RW_COUNT = 0x7FFFF000`) are handled by
`advance_vma_io_retry` which advances iovecs and resubmits. The 128 limit is
conservative; real workloads show 1–19 ios per process.

**auto_dedup**: `fallocate(PUNCH_HOLE)` is called after each AIO completion
when `args->auto_dedup` is set, at `cb->aio_offset` for `events[k].res`
bytes.

### 4. AIO vs io_uring review guidance

When explaining PR #3022, be precise:

- Do not claim `io_uring_queue_exit()` synchronously removes all helper
  threads. `close(ring_fd)` starts teardown; worker TIDs can remain visible
  until the queued exit work runs and any in-flight syscall returns.
- Do not say the AIO path falls back to `preadv`. The restorer still uses
  `io_submit`; if `O_DIRECT` is unavailable, buffered Linux AIO can execute
  synchronously in-kernel, which is the same blocking class as the old `preadv`
  path.
- SQPOLL is not a fix for this restorer problem. It changes submission and can
  add an `iou-sqp-*` thread; it does not remove `iou-wrk-*` workers for reads
  that cannot complete inline.

### 5. Pipeline C private-page overlap (`add-stream-restore-async`)

For stream-restore private VMAs, do not revive the old shared-futex design
without re-proving the address-space lifetime. PIE's `unmap_old_vmas` does
not preserve arbitrary `RM_SHARED` mappings, so a shmalloc-backed futex array
allocated late in `prepare_vma_ios()` is not a safe cross-process handoff.

The current overlap path uses a per-memfd readiness pipe instead:
`recv_streamer_private_fd()` receives `SCM_RIGHTS(memfd, pipe_rfd)` and an
immediate `'A'` ack, then PIE blocks on `sys_read(pipe_rfd, 1)` immediately
before consuming the memfd. Streamer close-without-write is the abort signal.

For UFFD minor-fault zero-copy, remember that `UFFDIO_CONTINUE` has no source
fd argument. The kernel resolves bytes from the faulting VMA's own
`vm_file + vm_pgoff`, so it only avoids copies when that VMA is already backed
by the memfd inode containing the bytes. It cannot populate a fresh CRIU-created
memfd inode from a different streamer memfd without some separate page-cache
clone or byte copy.

## Key Design Concepts

### The Restore Stage Barrier

CRIU uses a futex-based stage machine (`nr_in_progress` in `task_entries`).
The coordinator calls `restore_wait_inprogress_tasks()` which blocks until
`nr_in_progress == 0`. The root restored task advances from CR_STATE_FORKING
to CR_STATE_RESTORE, resetting `nr_in_progress` to all-tasks count. The
coordinator is blocked until ALL tasks complete both FORKING and RESTORE
stages. RESTORE is decremented from within the PIE restorer.

**Consequence**: `attach_to_tasks()` → `parse_threads()` runs only after all
tasks have entered their PIE restorers, which is after `open_vmas` (and all
pthread workers) have completed. No race between pthread workers and
`parse_threads`.

### `nr_threads` vs `nr_threads_image` (`criu/include/pstree.h`)

`nr_threads_image` is set once from the image at parse time and frozen.
`nr_threads` may be updated by `parse_threads()` (which reads live threads).
`core[]` and `rseqe[]` arrays are sized by `nr_threads_image`. A
`BUG_ON(nr_threads != nr_threads_image)` in `attach_to_tasks()` guards
against future ordering violations.

### `filemap_ctx` — Still Alive, But Narrowed

`filemap_ctx` (a per-process fd cache in `files-reg.c`) is still used by
`premap_priv_vmas` for premapping file-backed private VMAs early. In the new
`open_vmas`, filemap VMAs go into `unique[]` instead of calling `vm_open`,
so `filemap_ctx_init/fini` calls were removed from `open_vmas` Phase 1 (they
were no-ops).

### memfd vs shmem vs private VMA — What Gets Restored Where

| Memory type | Restored by | When |
|-------------|-------------|------|
| memfd-backed shmem (GPU buffers) | `prepare_memfd_inodes` → `restore_memfd_shmem_content_ex` → `shmem_restore_async` | Before `open_vmas`, in CRIU C code |
| Anonymous shmem (SysV, tmpfs) | `open_shmem` → `shmem_restore_async` | Before PIE restorer |
| Private VMA dirty pages | PIE restorer AIO (`vma_ios_fd` + `io_submit`) | Inside PIE restorer, after `mmap(MAP_FIXED)` |

Private pages must be read inside the PIE restorer because CRIU unmaps
itself before the target VMAs can be placed at their original addresses.

### O_DIRECT Strategy

O_DIRECT is enabled via `fcntl(F_SETFL, fl | O_DIRECT)` **after** the image
file is opened (not at open time), because `bfd` reads the image header using
heap-allocated unaligned buffers. O_DIRECT is added after the header read.

An NFS probe (`pread` aligned 4096-byte read at offset 0) checks whether the
server rejects O_DIRECT at read time (Azure NFS accepts `F_SETFL` but returns
`EINVAL` on `pread`). On rejection: fall back to buffered I/O with
`posix_fadvise(SEQUENTIAL)`.

`bool use_direct` is cached in `struct page_read` to avoid a `fcntl(F_GETFL)`
syscall per page read.

**Same-host warm-cache regression (CRIU #3053).** When dump and restore run on
the same host, O_DIRECT restore is *slower*, not faster: dump writes
`pages-N.img` buffered (`page-xfer.c` `write_pages_loc`), leaving the data dirty
in the page cache; restore reopens the same inode and flips the fd to O_DIRECT
(`probe_pages_o_direct`, `pagemap.c:803`), which bypasses that warm cache and
forces disk reads. Measured on `maps04`: buffered ~1.24s vs O_DIRECT ~8.3s
(~6.7×); `fsync` before the O_DIRECT read does not help (it is a cache-bypass,
not a writeback stall). Keep O_DIRECT for the cold cross-host/NVMe case but
prefer buffered + AIO when the image was just written on the same host.

**Fix (upstream PR #3066).** O_DIRECT is now opt-in via an `--image-io-mode`
option (renamed from `--cache`) that applies to both dump and restore, over CLI
or RPC: `--image-io-mode=writeback` (default) = buffered, byte-for-byte the
pre-#3022 behavior; `--image-io-mode=direct` = O_DIRECT on both dump write and
restore read. The restore side gates at the probe's *callers*
(`open_page_read_at()` and the vma-io builder in `mem.c`), not inside
`probe_pages_o_direct_read` itself. The dump side sets O_DIRECT on the
pages-image fd so the existing `splice()` in `write_pages_loc()` does zero-copy
direct I/O (`iter_file_splice_write` → `->write_iter` honors O_DIRECT on
ext4/xfs — this is *not* a 6.5-only feature; the 6.5 `ITER_PIPE` work was the
splice *read* path); a filesystem that can't do it returns `EFAULT`/`EINVAL`,
which triggers a buffered fallback. No bounce buffer and no kernel-version
check (the earlier `write_pages_o_direct` / `probe_pages_o_direct_write` bounce
buffer was deleted per avagin's review). Pushed as `d858d2503`.

### `open_path` Locking (`criu/files-reg.c`)

Per-rfi locking replaces the old global `remap_open_lock`:

- `reg_file_runtime` (heap, `pthread_mutex_t`): per-file runtime state for
  same-process parallel opens; holds `size_mode_checked` and `remap_cached_fd`
- `remap_rfi_lock` (shmem, `mutex_t`): per-file remap lock that survives
  fork; serializes the link-create / open / unlink sequence

The `remap_cached_fd` fast path: after the first opener does the link/open/
unlink dance, it caches the fd. Concurrent openers `dup()` it. **Critical**:
`dup()` must happen while `rt->lock` is held, because `restore_fown` failure
on the first opener closes the fd and clears the cache under the same lock.

## Performance Numbers (nscale, Qwen3-0.6B, sleep-mode checkpoint)

| Metric | Value |
|--------|-------|
| Checkpoint size | ~3.4 GB (GPU memory freed in sleep mode) |
| Memfd regions | 122 unique inodes |
| open_vmas (parallel, 8 workers) | ~2.4s |
| PIE restorer AIO (7 ios, 4.9 GB) | ~1.6s |
| pagemap async preadv | ~7–12 ms per region |
| Total restore | ~5s (nsrestore architecture) |

For larger models (gpt120b): 417 memfd regions, 122.9 GiB total, 13.7s open_vmas.
Memfd sizes range from 2 MiB to 2048 MiB; 39 × 2048 MiB dominate (63% of data).

## Review Fixes (commit `d942fe224`)

The following bugs were found and fixed during human review:

- **`__NR_fadvise64` dead entry** removed from `compel/arch/x86/syscall_64.tbl`
  (no PIE caller)
- **`dup()` race** in `open_path` fast path: `dup(cached)` now happens while
  `rt->lock` is held to prevent duping a fd being closed by a concurrent
  `restore_fown` failure
- **`remap_lock` redundant lookup** eliminated — pointer stored at function scope
- **Stale comment** "only visible in current process" removed from size/mode check
- **`bool use_direct`** added to `struct page_read` — eliminates `fcntl(F_GETFL)`
  per page read
- **O_DIRECT clear failure** upgraded from `pr_warn` to `pr_err + return -1` in
  both `open_page_read_fd_at` and `prepare_vma_ios`
- **`filemap_ctx_init/fini`** no-op calls removed from `open_vmas`
- **`ftruncate` ordering** fixed in `restore_memfd_shmem_content_ex`: open
  page_read before truncating, so a missing image doesn't silently truncate
  the caller's fd; redundant `ftruncate` in `open_shmem` removed
- **`auto_dedup` hole punching** restored in AIO event loop (was dropped in
  the preadv→AIO conversion)
- **`io_destroy`** called on mmap failure after `io_setup` succeeds
- **`aio_error:` label** added — all error paths inside AIO loop now clean up
  (`io_destroy + munmap + close fd`) before `goto core_restore_end`
- **Unused POSIX_FADV defines** removed from `pie/restorer.c`
- **`dump_threads_and_stacks`** diagnostic function removed from CUDA plugin
- **fd leak** in `open_vmas_unique_open` fixed — closes opened fds on error

## Known Remaining Items

- `pthread_mutex_destroy` never called on `reg_file_runtime` entries — minor
  leak on cleanup paths (process exits on restore failure anyway)
- `AIO_BATCH=128` is over-provisioned; real workloads show max 19 ios
- The `BUG_ON(nr_threads == nr_threads_image)` invariant is defensive — the
  barrier provably prevents the race, but the assertion is correct to have

---

# Checkpoint Image Format

## Image File Types Read During Restore

| Image type constant | File on disk | Contains | Opened by |
|--------------------|-------------|----------|-----------|
| `CR_FD_PAGEMAP` | `pagemap-N.img` | `PagemapEntry` items: vaddr, nr_pages, flags, offset into pages file | `open_page_read(id, PR_TASK)` |
| `CR_FD_SHMEM_PAGEMAP` | `pagemap-shmem-N.img` | Same but for shmem/memfd regions | `open_page_read(shmid, PR_SHMEM)` |
| `CR_FD_PAGES` | `pages-N.img` | Raw page data — all dirty pages concatenated | `open_pages_image_at()` via page_read |
| `CR_FD_MEMFD_INODE` | `memfd.img` | `MemfdInodeEntry`: name, shmid, size, seals, uid/gid | `collect_image()` in `prepare_memfd_inodes()` |
| `CR_FD_MM` | `mm-N.img` | `MmEntry`: exe file, brk, start_code, vmas list | `prepare_mm()` |
| `CR_FD_VMA` | embedded in `mm-N.img` | `VmaEntry` per mapping: start/end, flags, pgoff, fd | read as part of `MmEntry` |
| `CR_FD_CORE` | `core-N.img` | Thread registers, signal state, personality | `open_cores()` |
| `CR_FD_IDS` | `ids-N.img` | Namespace IDs, uid_map, gid_map | `prepare_task_entries()` |
| `CR_FD_FDINFO` | `fdinfo-N.img` | Per-fd metadata (type, flags, pos) | `prepare_fds()` |
| `CR_FD_REG_FILES` | `reg-files.img` | Regular file entries: path, flags, mode | `collect_remaps_and_regfiles()` |
| `CR_FD_GHOST_FILE` | `ghost-N.img` | Content of deleted-but-open files | `open_remap_ghost()` |
| `CR_FD_REMAP_FPATH` | `remap-fpath.img` | Remap entries linking fds to ghost/linked files | `collect_remaps_and_regfiles()` |
| `CR_FD_SIGNALFD` | `signalfd.img` | Signal fd state | `prepare_fds()` |

## `PagemapEntry` Fields

```protobuf
message PagemapEntry {
    uint64 vaddr     = 1;   // Virtual address of first page
    uint32 nr_pages  = 2;   // Consecutive pages in this run
    bool   in_parent = 3;   // Pages are in parent snapshot (skip reading)
    uint32 flags     = 4;   // PE_LAZY, PE_PARENT, etc.
}
```

The pagemap and pages files work together: for each `PagemapEntry` with `in_parent=false`,
read `nr_pages * PAGE_SIZE` bytes from `pages-N.img` at the current sequential offset.

---

# Function Call Trees

## 1. Top-Level Restore Flow

```
restore_one_alive_task(pid, core)              cr-restore.c:628
├─ prepare_fds(current)
├─ prepare_file_locks(pid)
├─ open_vmas(current)                          mem.c:1676  ← see §2
├─ prepare_aios(current, ta)
├─ fixup_sysv_shmems()
├─ open_cores(pid, core)
├─ prepare_signals / timers / rlimits / ...
├─ prepare_mm(pid, ta)
├─ prepare_vmas(current, ta)
│  └─ prepare_vma_ios(t, ta)                   mem.c:1817  ← see §5
└─ sigreturn_restore(pid, ta, ...)             → PIE restorer  ← see §6
```

## 2. `open_vmas` — 4-Phase Pipeline

```
open_vmas(t)                                   mem.c:1676
│
├─ [Phase 1] Walk VMAs:
│  ├─ shmem/socket/anon → vma->vm_open(pid, vma) immediately
│  │   open_shmem()     shmem.c:588   ← see §3
│  │   open_shmem_sysv() shmem.c:398
│  │   open_socket_map() sk-packet.c:353
│  └─ file/memfd VMAs → collect into unique[] (dedup by vmfd+flags)
│     VMA_EXT_PLUGIN → vm_open() directly (skip parallel path)
│
├─ [Phase 2] open_vmas_unique_open(unique[], n)  mem.c:1647
│  ├─ memfd_open_parallel(unique, n)             mem.c:1551  ← see §4
│  └─ [remaining non-memfd] open_file_for_vma(rep_vma, flags)
│                                                files-reg.c:2702
│                            └─ open_path(vmfd, do_open_reg_noseek_flags, &flags)
│
├─ [Phase 3] Assign vma->e->fd = unique[j].fd
│
└─ [Phase 4] Walk list → set VMA_CLOSE on last VMA per fd
```

Note: `filemap_ctx` / `open_filemap` are used only by `premap_priv_vmas`
(the pre-restore premapping path) — NOT by `open_vmas` Phase 2.

## 3. `open_shmem` — Anonymous Shared Memory

```
open_shmem(t, vma)                             shmem.c:588
│
├─ [Already created by another process] shmem_wait_and_open()
│
├─ [memfd available] memfd_create()
│  ├─ restore_memfd_shmem_content_ex(f, shmid, size)
│  │   ├─ open_page_read(shmid, &pr, PR_SHMEM)  ← see §7
│  │   ├─ ftruncate(fd, size)
│  │   ├─ mmap(fd, MAP_SHARED)  → addr
│  │   ├─ shmem_restore_async(&pr, addr, size)  ← see §8
│  │   └─ munmap + pr.close()
│  └─ si->fd = f
│
└─ [no memfd] mmap(MAP_ANONYMOUS)
   ├─ do_restore_shmem_content_ex(addr, size, shmid)
   │   ├─ open_page_read(shmid, &pr, PR_SHMEM)
   │   └─ shmem_restore_async(&pr, addr, size)
   └─ userns_call(open_map_file) → si->fd
```

## 4. `memfd_open_parallel` — Pthread Worker Pool

```
memfd_open_parallel(unique[], n)               mem.c:1551
│
├─ memfd_open_jobs_collect()    dedup by inode (memfd_inode_cookie)
│
├─ [0–1 jobs] sequential fallback
│
└─ [≥2 jobs, ≥2 CPUs] pthread pool
   │
   ├─ pthread_create × min(nr_jobs, CPUs, 8)
   │  └─ memfd_open_worker(plan)
   │     └─ loop: claim job (next_job++ under lock), then:
   │        ├─ inherited_fd() — fast path if already open
   │        └─ memfd_open(vmfd, flags, filemap=true)  memfd.c:334
   │           ├─ memfd_open_inode(mfi->inode)
   │           │  ├─ [fast] fdstore_id != -1 → fdstore_get()
   │           │  └─ [slow] mutex_lock → memfd_open_inode_nocache()
   │           │               ├─ memfd_create(name, flags)   [syscall]
   │           │               ├─ restore_memfd_shmem_content(fd, shmid, size)
   │           │               │   └─ restore_memfd_shmem_content_ex()  ← see §3
   │           │               ├─ cr_fchperm(fd, uid, gid, mode)
   │           │               └─ fdstore_add(fd)
   │           └─ reopen with correct flags via /proc/self/fd/N
   │
   └─ pthread_join × workers
```

## 5. `prepare_vma_ios` — Set Up PIE Restorer I/O

```
prepare_vma_ios(t, ta)                         mem.c:1817
│
├─ [Empty vma_io list] → ta->vma_ios_fd = -1, return
│
├─ open_image(CR_FD_PAGES, O_RDWR|O_RSTR, pages_img_id)
│  └─ ta->vma_ios_fd = img_raw_fd(pages)
│
├─ fcntl(ta->vma_ios_fd, F_SETFL, O_DIRECT)   [try O_DIRECT]
├─ NFS probe: pread() 4KB aligned at offset 0
│  ├─ EINVAL → F_SETFL clear + posix_fadvise(SEQUENTIAL)
│  └─ success → O_DIRECT stays on
│
└─ pagemap_render_iovec(&rsti(t)->vma_io, ta)
   └─ Build ta->vma_ios[]: one restore_vma_io per pagemap region
      Each entry: { nr_iovs, off (file offset), iovs[] (dest VMA addrs) }
```

## 6. PIE Restorer — AIO Private Page Reads

```
[PIE restorer, running inside restored process after CRIU unmaps itself]
[vma_ios_fd, vma_ios[], vma_ios_n from task_restore_args]

io_setup(AIO_BATCH=128, &aio_ctx)             [raw syscall]

[Build iocbs: one per restore_vma_io entry]
for i in 0..vma_ios_n:
    iocb[i] = {
        aio_lio_opcode = IOCB_CMD_PREADV,
        aio_fildes     = vma_ios_fd,
        aio_buf        = (uint64) rio->iovs,   [iovec array ptr]
        aio_nbytes     = rio->nr_iovs,         [iovec count]
        aio_offset     = rio->off,             [file offset]
        aio_data       = expected_bytes,       [completion marker]
    }

[Sliding window: submit up to AIO_BATCH, reap completions]
while submitted < n or completed < n:
    submit: io_submit(aio_ctx, batch, iocbps[submitted..])
    reap:   io_getevents(aio_ctx, 1, AIO_BATCH, events)
    for each event:
        [auto_dedup] → fallocate(PUNCH_HOLE, cb->aio_offset, res)
        [complete]   → completed++
        [short read] → advance_vma_io_retry(); io_submit retry
        [error/EOF]  → goto aio_error

[cleanup]
aio_error: io_destroy(aio_ctx); munmap(iocbs); close(fd)
aio_done:  io_destroy(aio_ctx); munmap(iocbs); close(fd); log timing
```

## 7. `open_page_read` — Image File Setup

```
open_page_read(img_id, &pr, flags)             pagemap.c:997
└─ open_page_read_at(IMG_FD_OFF, img_id, &pr, flags)
   │
   ├─ INIT_LIST_HEAD(&pr->async)
   │
   ├─ pr->pmi = open_image_at(dfd, CR_FD_PAGEMAP or CR_FD_SHMEM_PAGEMAP, img_id)
   │            ← INDEX file: which pages exist and where in pages-N.img
   │
   ├─ try_open_parent() — snapshot chain (incremental checkpoint parent)
   │   └─ open_page_read_at(parent_dfd, ...) → pr->parent
   │
   ├─ pr->pi = open_pages_image_at(dfd, flags, pr->pmi, &pr->pages_img_id)
   │           ← DATA file: raw page bytes (CR_FD_PAGES)
   │
   ├─ [O_DIRECT setup — see O_DIRECT Strategy section]
   │   └─ set pr->use_direct = true on success
   │
   ├─ init_pagemaps(pr) — decode all PagemapEntries into pr->pmes[]
   │
   └─ vtable setup:
       pr->read_pages      = read_pagemap_page
       pr->advance         = advance           [steps through pr->pmes[]]
       pr->sync            = process_async_reads
       pr->close           = close_page_read
       pr->reset           = reset_pagemap
       pr->seek_pagemap    = seek_pagemap
       pr->maybe_read_page = maybe_read_page_local          [default]
                           | maybe_read_page_img_streamer   [--stream mode]
                           | maybe_read_page_remote         [--remote / page server]
```

## 8. `shmem_restore_async` — Async Read Loop

```
shmem_restore_async(&pr, addr, size)           shmem.c:472
│
├─ loop:
│  ├─ pr->advance(pr)       → next PagemapEntry in pr->pmi
│  │  [returns 0 = done, -1 = error, 1 = entry ready]
│  │
│  ├─ bounds check: vaddr + nr_pages*PAGE_SIZE <= size
│  │
│  └─ pr->read_pages(pr, vaddr, nr_pages, addr+vaddr, PR_ASYNC)
│     └─ read_pagemap_page()
│        └─ maybe_read_page(pr, vaddr, nr, buf, PR_ASYNC)
│           └─ maybe_read_page_local()
│              ├─ [PR_ASYNC only, not PR_ASAP]
│              │  → pagemap_enqueue_iovec(pr, buf, len, &pr->async)
│              │     Coalesce consecutive iovecs into pr->bunch,
│              │     flush to pr->async list when non-contiguous
│              └─ pr->pi_off += len
│
└─ pr->sync(pr)              → process_async_reads()    pagemap.c:577
   │
   ├─ fd = img_raw_fd(pr->pi)
   ├─ posix_fadvise(fd, [min_off, max_off], POSIX_FADV_WILLNEED)
   ├─ [loop over pr->async list]:
   │  └─ preadv(fd, piov->to, piov->nr, piov->from)
   │     ← reads all enqueued iovecs in one syscall per region
   │     short read → advance_piov() → retry
   │     auto_dedup → fallocate(PUNCH_HOLE, from, bytes_read)
   └─ [recurse into pr->parent if snapshot chain]
```

## 9. `struct page_read` Vtable Summary

| Pointer | Implementations | Purpose |
|---------|----------------|---------|
| `read_pages` | `read_pagemap_page` | Routes to `maybe_read_page` or parent |
| `advance` | `advance` (pagemap.c) | Steps pr->pe to next PagemapEntry |
| `sync` | `process_async_reads` | Flushes async queue via preadv |
| `close` | `close_page_read` | Closes pmi + pi, logs direct-io stats |
| `reset` | `reset_pagemap` | Resets cvaddr/pi_off/curr_pme to 0 |
| `seek_pagemap` | `seek_pagemap` | Binary search pr->pmes[] for vaddr |
| `io_complete` | NULL or caller-set | Post-read callback (e.g. for UFFD) |
| `maybe_read_page` | `maybe_read_page_local` (default) | Enqueue (PR_ASYNC) or read sync |
| | `maybe_read_page_img_streamer` | Sequential `read()` for `--stream` mode (no seek) |
| | `maybe_read_page_remote` | Remote page server (`--remote`) |

## 10. Thread / Process Lifecycle During Restore

```
CRIU coordinator (main process)
│
├─ crtools_prepare_shared()       ← single-threaded
│  ├─ prepare_memfd_inodes()      ← loads metadata only (lazy open)
│  ├─ prepare_files()
│  └─ prepare_remaps()
│
├─ fork() restored process tree   ← CR_STATE_FORKING barrier
│  │
│  └─ Each restored process (in forked child):
│     ├─ restore_one_alive_task()
│     │  ├─ open_vmas()
│     │  │  └─ pthread_create × 8 workers  ← PARALLEL HERE
│     │  │     pthread_join (all joined before open_vmas returns)
│     │  └─ sigreturn_restore()  → PIE restorer (single-threaded again)
│     │
│     └─ PIE restorer decrement CR_STATE_RESTORE counter
│
└─ [after all processes decrement] coordinator unblocks
   └─ attach_to_tasks()  ← all pthreads long gone by here
      └─ parse_threads() + ptrace SEIZE
```
