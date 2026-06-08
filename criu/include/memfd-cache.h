#ifndef __CR_MEMFD_CACHE_H__
#define __CR_MEMFD_CACHE_H__

#include <stdint.h>
#include <stdbool.h>

#include "images/memfd.pb-c.h"

/*
 * Node-local memfd content cache -- CRIU (client) side.
 *
 * The snapshot-agent runs a per-node cache server that holds populated, sealed
 * memfds open so a second restore of the same checkpoint on the same node can
 * borrow the already-filled inode instead of re-reading pages-<shmid>.img and
 * copying them into a fresh memfd. CRIU (swrk restore) is the client.
 *
 * Transport: one AF_UNIX SOCK_SEQPACKET socketpair brackets one restore. The
 * agent holds one end and services it; the other end is inherited by criu via
 * the CRIU_MEMFD_CACHE_SOCK env var (fd number), exactly like the streamer
 * sockets. SCM_RIGHTS over an inherited socketpair is namespace-agnostic, so no
 * socket bind-mounting across the placeholder mount namespace is needed.
 * Closing the criu end (restore complete) releases every borrow taken on it.
 *
 * Wire format: fixed-layout little-endian structs (the agent and criu always
 * run on the same node, same arch). Negotiation messages (req/resp) are plain
 * SEQPACKET datagrams; an fd only ever crosses via SCM_RIGHTS, never on a MISS
 * (recv_fds cannot represent a zero-fd message).
 *
 * Sharing safety: only inodes sealed F_SEAL_FUTURE_WRITE are cached/borrowed.
 * The kernel then forbids any writable mapping of the inode, so the single
 * shared physical copy cannot be corrupted by any borrower. uid/gid are part of
 * the key, so a HIT fd is always already owned correctly (a memfd inode has one
 * owner); a different owner is a clean MISS that fills its own copy.
 */

#define MEMFD_CACHE_ID_MAX 128

struct memfd_cache_key {
	uint64_t shmid;			 /* MemfdInodeEntry.shmid: dump-time inode #, frozen in the image */
	uint32_t uid;			 /* resolved owner uid (load-bearing, see header comment) */
	uint32_t gid;			 /* resolved owner gid */
	char id[MEMFD_CACHE_ID_MAX];	 /* "checkpointID:version", NUL-padded scope */
};

enum memfd_cache_op {
	MEMFD_CACHE_OP_GET = 1,		 /* client -> server: do you hold this key? */
	MEMFD_CACHE_OP_DONATE = 2,	 /* client -> server: here is a sealed populated fd */
};

struct memfd_cache_req {
	uint32_t op;			 /* enum memfd_cache_op */
	uint32_t seals;			 /* donor seals (must include F_SEAL_FUTURE_WRITE) */
	uint64_t size;			 /* memfd size: HIT validator / donate size */
	struct memfd_cache_key key;
};

enum memfd_cache_status {
	MEMFD_CACHE_HIT = 1,		 /* GET reply: fd follows via SCM_RIGHTS */
	MEMFD_CACHE_MISS = 2,		 /* GET reply: no fd */
	MEMFD_CACHE_OK = 3,		 /* DONATE reply: accepted */
	MEMFD_CACHE_DECLINE = 4,	 /* DONATE reply: declined (over budget) */
};

struct memfd_cache_resp {
	uint32_t status;		 /* enum memfd_cache_status */
	uint32_t _pad;
};

/* memfd_cache_get() result. */
enum memfd_cache_result {
	MEMFD_CACHE_R_MISS = 0,	 /* no entry: caller fills locally */
	MEMFD_CACHE_R_HIT = 1,	 /* *out_fd holds a populated, sized, sealed, correctly-owned fd */
	MEMFD_CACHE_R_ERR = -1,	 /* transport error: caller fills locally AND should stop using the socket */
};

/*
 * Cache socket fd from CRIU_MEMFD_CACHE_SOCK, or -1 when absent. Cached after
 * the first call. The cache is active only when this is >= 0, opts.memfd_cache
 * is set, and opts.memfd_cache_id is non-empty.
 */
int memfd_cache_sock(void);

/* True if this inode is shareable: sealed F_SEAL_FUTURE_WRITE and not hugetlb. */
bool memfd_cache_eligible(MemfdInodeEntry *mie);

/*
 * GET the cached fd for key(mie, cache_id). On MEMFD_CACHE_R_HIT *out_fd is a
 * populated, sized, F_SEAL_FUTURE_WRITE-sealed fd owned by mie->uid/gid; the
 * caller owns it. On MISS/ERR *out_fd is left untouched.
 */
enum memfd_cache_result memfd_cache_get(int sock, MemfdInodeEntry *mie, const char *cache_id, int *out_fd);

/*
 * DONATE a now-sealed populated fd for key(mie, cache_id). Best-effort: returns
 * 0 on a clean exchange (OK or DECLINE) and -1 on a transport error (caller
 * should stop using the socket). A declined donation is not an error: the
 * caller already has its own filled copy.
 */
int memfd_cache_donate(int sock, int fd, MemfdInodeEntry *mie, const char *cache_id);

#endif /* __CR_MEMFD_CACHE_H__ */
