/*
 * block.c — Block I/O layer with LRU buffer cache.
 *
 * Design:
 *   - A hash table keyed by block number gives O(1) lookup.
 *   - A doubly-linked LRU list tracks recency; the tail is the
 *     least-recently-used candidate for eviction.
 *   - Pin counting (b_refcnt) prevents eviction of in-use buffers.
 *   - Dirty buffers are written via buf_bwrite() or bdev_sync().
 *   - The superblock is read/written directly via bdev_pread/bdev_pwrite,
 *     not through the cache.
 *
 * Thread safety: bd_lock serialises all cache operations.  Callers that
 * hold a struct buf pointer (via buf_bread/getblk) must call brelse() to
 * decrement the pin count; the lock is NOT held between buf_bread and brelse.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>

#include "block.h"

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static inline uint32_t
bhash(struct bdev *bd, int64_t blkno)
{
    return (uint32_t)((blkno ^ (blkno >> 16)) & bd->bd_hashmask);
}

/* Find a buffer in the cache. bd_lock MUST be held by caller. */
static struct buf *
bcache_lookup(struct bdev *bd, int64_t blkno)
{
    struct buf *bp;
    LIST_FOREACH(bp, &bd->bd_hashtbl[bhash(bd, blkno)], b_hash) {
        if (bp->b_blkno == blkno)
            return bp;
    }
    return NULL;
}

/*
 * Move bp to the head of the LRU list (most-recently-used).
 * bd_lock MUST be held.
 */
static void
bcache_touch(struct bdev *bd, struct buf *bp)
{
    TAILQ_REMOVE(&bd->bd_lru, bp, b_lru);
    TAILQ_INSERT_HEAD(&bd->bd_lru, bp, b_lru);
}

/*
 * Evict the least-recently-used clean buffer.
 * Returns 0 on success, -1 if no evictable buffer found.
 * bd_lock MUST be held.
 */
static int
bcache_evict(struct bdev *bd)
{
    struct buf *bp;

    TAILQ_FOREACH_REVERSE(bp, &bd->bd_lru, buf_lru_head, b_lru) {
        if (bp->b_refcnt == 0 && !(bp->b_flags & B_DIRTY))
            goto found;
    }
    /* All buffers are pinned or dirty; try flushing dirty ones */
    TAILQ_FOREACH_REVERSE(bp, &bd->bd_lru, buf_lru_head, b_lru) {
        if (bp->b_refcnt == 0 && (bp->b_flags & B_DIRTY))
            goto found;
    }
    return -1;

found:
    if (bp->b_flags & B_DIRTY) {
        /* Write it out before eviction (unlocked because pwrite is safe) */
        off_t off = (off_t)bp->b_blkno * bd->bd_blksize;
        ssize_t n = pwrite(bd->bd_fd, bp->b_data, bp->b_bcount, off);
        if (n < 0 || (size_t)n != bp->b_bcount) {
            /* Cannot evict — leave dirty in cache */
            return -1;
        }
        bp->b_flags &= ~(B_DIRTY | B_DELWRI);
    }

    TAILQ_REMOVE(&bd->bd_lru, bp, b_lru);
    LIST_REMOVE(bp, b_hash);
    free(bp->b_data);
    free(bp);
    bd->bd_nbufs--;
    return 0;
}

/*
 * Allocate a new buffer for block blkno of size bytes.
 * bd_lock MUST be held; may call bcache_evict.
 */
static struct buf *
bcache_alloc(struct bdev *bd, int64_t blkno, uint32_t size)
{
    struct buf *bp;

    /* Evict if at capacity */
    while (bd->bd_nbufs >= bd->bd_maxbufs) {
        if (bcache_evict(bd) != 0)
            break;  /* can't evict; proceed anyway (cache grows temporarily) */
    }

    bp = calloc(1, sizeof(*bp));
    if (bp == NULL)
        return NULL;
    bp->b_data = malloc(size);
    if (bp->b_data == NULL) {
        free(bp);
        return NULL;
    }
    bp->b_blkno  = blkno;
    bp->b_bcount = size;
    bp->b_refcnt = 1;
    bp->b_flags  = 0;

    TAILQ_INSERT_HEAD(&bd->bd_lru, bp, b_lru);
    LIST_INSERT_HEAD(&bd->bd_hashtbl[bhash(bd, blkno)], bp, b_hash);
    bd->bd_nbufs++;
    return bp;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct bdev *
bdev_open(const char *path, int rdonly, uint32_t blksize, uint32_t maxbufs)
{
    struct bdev *bd;
    int flags = rdonly ? O_RDONLY : O_RDWR;
    int fd;

    fd = open(path, flags);
    if (fd < 0)
        return NULL;

    bd = calloc(1, sizeof(*bd));
    if (bd == NULL) {
        close(fd);
        return NULL;
    }

    bd->bd_fd       = fd;
    bd->bd_blksize  = blksize;
    bd->bd_maxbufs  = (maxbufs > 0) ? maxbufs : BCACHE_DEFAULT_BUFS;

    /* Determine device size */
    off_t sz = lseek(fd, 0, SEEK_END);
    bd->bd_size = (sz > 0) ? (uint64_t)sz : 0;

    /* Hash table: power-of-2 sized, at least 256 entries */
    uint32_t hsize = 256;
    while (hsize < bd->bd_maxbufs)
        hsize <<= 1;
    bd->bd_hashmask = hsize - 1;
    bd->bd_hashtbl  = calloc(hsize, sizeof(bd->bd_hashtbl[0]));
    if (bd->bd_hashtbl == NULL) {
        free(bd);
        close(fd);
        return NULL;
    }
    for (uint32_t i = 0; i < hsize; i++)
        LIST_INIT(&bd->bd_hashtbl[i]);

    TAILQ_INIT(&bd->bd_lru);
    pthread_mutex_init(&bd->bd_lock, NULL);

    return bd;
}

void
bdev_close(struct bdev *bd)
{
    struct buf *bp, *tmp;

    pthread_mutex_lock(&bd->bd_lock);
    /* Flush dirty */
    TAILQ_FOREACH_SAFE(bp, &bd->bd_lru, b_lru, tmp) {
        if (bp->b_flags & B_DIRTY) {
            off_t off = (off_t)bp->b_blkno * bd->bd_blksize;
            (void)pwrite(bd->bd_fd, bp->b_data, bp->b_bcount, off);
        }
        free(bp->b_data);
        free(bp);
    }
    pthread_mutex_unlock(&bd->bd_lock);

    free(bd->bd_hashtbl);
    pthread_mutex_destroy(&bd->bd_lock);
    close(bd->bd_fd);
    free(bd);
}

int
bdev_sync(struct bdev *bd)
{
    struct buf *bp;
    int rc = 0;

    pthread_mutex_lock(&bd->bd_lock);
    TAILQ_FOREACH(bp, &bd->bd_lru, b_lru) {
        if (bp->b_flags & B_DIRTY) {
            off_t off = (off_t)bp->b_blkno * bd->bd_blksize;
            ssize_t n = pwrite(bd->bd_fd, bp->b_data, bp->b_bcount, off);
            if (n < 0 || (size_t)n != bp->b_bcount)
                rc = -1;
            else
                bp->b_flags &= ~(B_DIRTY | B_DELWRI);
        }
    }
    pthread_mutex_unlock(&bd->bd_lock);

    if (fdatasync(bd->bd_fd) < 0)
        rc = -1;
    return rc;
}

struct buf *
buf_bread(struct bdev *bd, int64_t blkno, uint32_t size)
{
    struct buf *bp;

    pthread_mutex_lock(&bd->bd_lock);
    bp = bcache_lookup(bd, blkno);
    if (bp != NULL) {
        bp->b_refcnt++;
        bcache_touch(bd, bp);
        pthread_mutex_unlock(&bd->bd_lock);
        return bp;
    }

    /* Cache miss — allocate and populate */
    bp = bcache_alloc(bd, blkno, size);
    if (bp == NULL) {
        pthread_mutex_unlock(&bd->bd_lock);
        return NULL;
    }
    pthread_mutex_unlock(&bd->bd_lock);

    /* Perform the read outside the lock (pread is thread-safe) */
    off_t off = (off_t)blkno * bd->bd_blksize;
    ssize_t n = pread(bd->bd_fd, bp->b_data, size, off);
    if (n < 0 || (size_t)n != size) {
        /* Failed read: put back a clean zeroed buffer */
        memset(bp->b_data, 0, size);
    }
    return bp;
}

struct buf *
getblk(struct bdev *bd, int64_t blkno, uint32_t size)
{
    struct buf *bp;

    pthread_mutex_lock(&bd->bd_lock);
    bp = bcache_lookup(bd, blkno);
    if (bp != NULL) {
        bp->b_refcnt++;
        bcache_touch(bd, bp);
        pthread_mutex_unlock(&bd->bd_lock);
        return bp;
    }
    bp = bcache_alloc(bd, blkno, size);
    if (bp != NULL)
        memset(bp->b_data, 0, size);
    pthread_mutex_unlock(&bd->bd_lock);
    return bp;
}

void
bdirty(struct buf *bp)
{
    bp->b_flags |= B_DIRTY;
}

void
bdwrite(struct buf *bp)
{
    bp->b_flags |= B_DIRTY | B_DELWRI;
}

int
buf_bwrite(struct bdev *bd, struct buf *bp)
{
    off_t off = (off_t)bp->b_blkno * bd->bd_blksize;
    ssize_t n = pwrite(bd->bd_fd, bp->b_data, bp->b_bcount, off);
    if (n < 0 || (size_t)n != bp->b_bcount)
        return -1;
    bp->b_flags &= ~(B_DIRTY | B_DELWRI);
    brelse(bd, bp);
    return 0;
}

void
brelse(struct bdev *bd, struct buf *bp)
{
    pthread_mutex_lock(&bd->bd_lock);
    if (bp->b_refcnt > 0)
        bp->b_refcnt--;
    pthread_mutex_unlock(&bd->bd_lock);
}

int
bdev_pread(struct bdev *bd, void *buf, size_t len, off_t off)
{
    ssize_t n = pread(bd->bd_fd, buf, len, off);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

int
bdev_pwrite(struct bdev *bd, const void *buf, size_t len, off_t off)
{
    ssize_t n = pwrite(bd->bd_fd, buf, len, off);
    if (n < 0 || (size_t)n != len)
        return -1;
    return 0;
}

void
binval(struct bdev *bd)
{
    struct buf *bp, *tmp;

    pthread_mutex_lock(&bd->bd_lock);
    TAILQ_FOREACH_SAFE(bp, &bd->bd_lru, b_lru, tmp) {
        if (bp->b_refcnt == 0) {
            TAILQ_REMOVE(&bd->bd_lru, bp, b_lru);
            LIST_REMOVE(bp, b_hash);
            free(bp->b_data);
            free(bp);
            bd->bd_nbufs--;
        }
    }
    pthread_mutex_unlock(&bd->bd_lock);
}
