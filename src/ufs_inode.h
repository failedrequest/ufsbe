/*
 * ufs_inode.h — In-core inode representation.
 * Derived from FreeBSD 15.1 sys/ufs/ufs/inode.h.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UFS_INODE_H
#define UFS_INODE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/queue.h>

#include "ufs_dinode.h"

/* ------------------------------------------------------------------ */
/* Forward declarations                                                 */
/* ------------------------------------------------------------------ */

struct ufs_mount;

/* ------------------------------------------------------------------ */
/* In-core inode flags (i_flag)                                        */
/* ------------------------------------------------------------------ */

#define IN_ACCESS   0x0001  /* access time update pending */
#define IN_CHANGE   0x0002  /* ctime update pending */
#define IN_UPDATE   0x0004  /* mtime update pending */
#define IN_MODIFIED 0x0008  /* inode dirty, must write */
#define IN_RENAME   0x0010  /* inode is being renamed */
#define IN_HASHED   0x0080  /* inode is in the hash table */
#define IN_LAZYMOD  0x0100  /* modified, but defer write */
#define IN_SIZEMOD  0x0200  /* file size has changed */
#define IN_SPACECOUNTED 0x0400 /* shadow inode space accounted */

/* ------------------------------------------------------------------ */
/* In-core inode                                                        */
/* ------------------------------------------------------------------ */

struct inode {
    LIST_ENTRY(inode)   i_hash;     /* hash chain by (mount, ino) */
    TAILQ_ENTRY(inode)  i_lru;      /* LRU chain for reclaim */

    struct ufs_mount   *i_ump;      /* owning mount */
    uint32_t            i_number;   /* inode number */
    uint32_t            i_flag;     /* IN_* flags */
    uint32_t            i_refcnt;   /* FUSE lookup/open ref count */
    int                 i_effnlink; /* effective nlink (softdep) */

    pthread_rwlock_t    i_lock;     /* per-inode rw lock */

    /* The on-disk dinode copy; mutually exclusive union for UFS1/UFS2 */
    union {
        struct ufs1_dinode  di1;
        struct ufs2_dinode  di2;
    } i_din;

    /* Soft-updates linkage (opaque pointer managed by softdep.c) */
    void               *i_sdep;
};

/* Convenience accessors — callers must know the filesystem version */
#define I_DI1(ip)   (&(ip)->i_din.di1)
#define I_DI2(ip)   (&(ip)->i_din.di2)

/* ------------------------------------------------------------------ */
/* Inode hash table                                                     */
/* ------------------------------------------------------------------ */

#define INODE_HASH_SIZE 1024
#define INODE_HASH(ump, ino) \
    (((uintptr_t)(ump) ^ (ino)) & (INODE_HASH_SIZE - 1))

LIST_HEAD(inode_hashhead, inode);

#endif /* UFS_INODE_H */
