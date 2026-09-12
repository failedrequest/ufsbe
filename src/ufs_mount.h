/*
 * ufs_mount.h — In-core mount state for ufs-fuse.
 * Derived from FreeBSD 15.1 sys/ufs/ufs/ufsmount.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UFS_MOUNT_H
#define UFS_MOUNT_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>

#include "ufs_fs.h"
#include "ufs_inode.h"
#include "ufs_block.h"

/* ------------------------------------------------------------------ */
/* Mount flags                                                          */
/* ------------------------------------------------------------------ */

#define UFS_MOUNT_RDONLY    0x0001  /* mounted read-only */
#define UFS_MOUNT_SOFTDEP   0x0002  /* soft updates enabled */
#define UFS_MOUNT_JOURNAL   0x0004  /* soft-update journaling enabled */
#define UFS_MOUNT_ASYNC     0x0008  /* async (no ordering guarantees) */
#define UFS_MOUNT_NEEDSFSCK 0x0010  /* fsck required at next mount */
#define UFS_MOUNT_UNCLEAN   0x0020  /* was not cleanly unmounted */
#define UFS_MOUNT_UFS2      0x0040  /* filesystem is UFS2 */
#define UFS_MOUNT_BETREE    0x0080  /* B-epsilon block map + dir index */

/* ------------------------------------------------------------------ */
/* In-core mount                                                        */
/* ------------------------------------------------------------------ */

struct ufs_mount {
    /* Device */
    int                 um_fd;          /* open file descriptor for device */
    char               *um_devpath;     /* path to device/image */
    uint64_t            um_devsize;     /* device size in bytes */

    /* Superblock — we keep a heap copy, not a buffer-cache entry,
     * because the SB is read/written specially */
    struct fs          *um_fs;          /* in-core superblock */
    int64_t             um_sboffset;    /* byte offset of SB we used */

    /* Cylinder-group summary cache */
    struct csum        *um_csmem;       /* malloc'd cg summary array */

    /* Flags */
    uint32_t            um_flags;       /* UFS_MOUNT_* */

    /* Derived constants (set from superblock at mount time) */
    uint64_t            um_nindir;      /* indirect ptrs per block */
    uint64_t            um_bptrtodb;    /* indir ptr to disk block shift */
    uint64_t            um_seqinc;      /* sequential block increment */

    /* Inode cache */
    struct inode_hashhead um_ihash[INODE_HASH_SIZE];
    TAILQ_HEAD(, inode) um_ilru;        /* LRU list for inode reclaim */
    uint32_t            um_ninode;      /* number of inodes in cache */
    pthread_mutex_t     um_ilock;       /* protects ihash + ilru + ninode */

    /* Block/buffer cache (managed by block.c) */
    struct bdev        *um_bdev;

    /* Soft-updates state (managed by softdep.c) */
    void               *um_sdep;

    /* Journal state (managed by journal.c) */
    void               *um_journal;

    /* Statistics */
    uint64_t            um_reads;
    uint64_t            um_writes;
    uint64_t            um_cache_hits;
    uint64_t            um_cache_misses;
};

/* ------------------------------------------------------------------ */
/* Accessor macros                                                      */
/* ------------------------------------------------------------------ */

#define UFS_FS(ump)     ((ump)->um_fs)
#define UFS_IS2(ump)    (((ump)->um_flags & UFS_MOUNT_UFS2) != 0)
#define UMP_RDONLY(ump) (((ump)->um_flags & UFS_MOUNT_RDONLY) != 0)
#define UFS_SOFTDEP(ump)(((ump)->um_flags & UFS_MOUNT_SOFTDEP) != 0)
#define UFS_JOURNAL(ump)(((ump)->um_flags & UFS_MOUNT_JOURNAL) != 0)
#define UFS_BETREE(ump) (((ump)->um_flags & UFS_MOUNT_BETREE) != 0)

#endif /* UFS_MOUNT_H */
