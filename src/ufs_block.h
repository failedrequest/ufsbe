/*
 * ufs_block.h — In-core buffer/block cache entry.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UFS_BLOCK_H
#define UFS_BLOCK_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/queue.h>

/* ------------------------------------------------------------------ */
/* Buffer flags                                                         */
/* ------------------------------------------------------------------ */

#define B_DIRTY     0x0001  /* buffer has been modified */
#define B_DELWRI    0x0002  /* delayed write (soft-updates deferred) */
#define B_LOCKED    0x0004  /* pinned: do not evict */
#define B_BARRIER   0x0008  /* must be written before subsequent writes */
#define B_NEEDCOMMIT 0x0010 /* journal: write to journal before data */

/* ------------------------------------------------------------------ */
/* In-core buffer                                                       */
/* ------------------------------------------------------------------ */

TAILQ_HEAD(buf_lru_head, buf);

struct buf {
    TAILQ_ENTRY(buf) b_lru;         /* LRU chain */
    LIST_ENTRY(buf)  b_hash;        /* hash chain by (dev, blkno) */

    int64_t          b_blkno;       /* file-system block number */
    uint32_t         b_bcount;      /* size of buffer in bytes */
    uint32_t         b_flags;       /* B_* flags */
    uint32_t         b_refcnt;      /* pin count (must reach 0 to evict) */

    uint8_t         *b_data;        /* pointer to data bytes */
};

/* ------------------------------------------------------------------ */
/* Forward declaration for the block device descriptor                  */
/* ------------------------------------------------------------------ */

struct bdev;

#endif /* UFS_BLOCK_H */
