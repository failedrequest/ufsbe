/*
 * block.h — Block I/O and buffer cache interface.
 *
 * The buffer cache sits between the filesystem logic and the raw device.
 * It provides:
 *   - LRU eviction with a configurable capacity
 *   - Dirty-tracking and write-back (both synchronous and delayed)
 *   - B_BARRIER ordering used by soft updates
 *   - Direct-I/O path for the superblock (not cached)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef BLOCK_H
#define BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include "ufs_block.h"

/* ------------------------------------------------------------------ */
/* Block device descriptor                                              */
/* ------------------------------------------------------------------ */

#define BCACHE_DEFAULT_BUFS 4096    /* default number of cached blocks */

struct bdev {
    int         bd_fd;              /* open fd for device/image */
    uint64_t    bd_size;            /* device size in bytes */
    uint32_t    bd_blksize;         /* logical block size (= fs_fsize) */
    uint32_t    bd_maxbufs;         /* buffer cache capacity */

    /* LRU queue: head = MRU, tail = LRU */
    struct buf_lru_head bd_lru;
    /* Hash table for O(1) lookup by block number */
    LIST_HEAD(bhashhead, buf) *bd_hashtbl;
    uint32_t            bd_hashmask;
    uint32_t            bd_nbufs;   /* current number of bufs in cache */

    pthread_mutex_t     bd_lock;
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* Open device/image and initialise buffer cache. */
struct bdev *bdev_open(const char *path, int rdonly, uint32_t blksize,
                       uint32_t maxbufs);

/* Release all buffers and close device. */
void bdev_close(struct bdev *bd);

/* Flush all dirty buffers to disk. */
int bdev_sync(struct bdev *bd);

/* Read a filesystem block (blkno in FS-block units, not byte offset).
 * Returns a locked (B_LOCKED-pinned) buf; call brelse() when done. */
struct buf *buf_bread(struct bdev *bd, int64_t blkno, uint32_t size);

/* Like buf_bread() but do not issue a read if block is not cached;
 * returns a zeroed fresh buffer (for newly allocated blocks). */
struct buf *getblk(struct bdev *bd, int64_t blkno, uint32_t size);

/* Mark buffer dirty; it will be written on bdev_sync() or bdwrite(). */
void bdirty(struct buf *bp);

/* Asynchronous (delayed) write: mark dirty + deferred. */
void bdwrite(struct buf *bp);

/* Synchronous write: write buffer immediately to disk, then release. */
int buf_bwrite(struct bdev *bd, struct buf *bp);

/* Release a buffer (decrement pin count; make eligible for eviction). */
void brelse(struct bdev *bd, struct buf *bp);

/* Low-level I/O bypassing cache (used for superblock). */
int bdev_pread(struct bdev *bd, void *buf, size_t len, off_t off);
int bdev_pwrite(struct bdev *bd, const void *buf, size_t len, off_t off);

/* Invalidate (discard) all cached buffers — used after journal recovery. */
void binval(struct bdev *bd);

#endif /* BLOCK_H */
