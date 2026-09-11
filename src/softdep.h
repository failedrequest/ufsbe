/*
 * softdep.h — Soft-updates dependency engine.
 *
 * Soft updates guarantees that on-disk metadata is always consistent
 * in the sense that no allocated structure points to a free resource.
 * This is achieved by tracking dependencies between writes and deferring
 * or ordering them so that:
 *
 *   1. A new inode is written to disk BEFORE the directory entry that
 *      points to it (so a crash cannot create a dangling pointer).
 *   2. A directory entry is REMOVED from disk BEFORE the inode's nlink
 *      is decremented to zero and the inode is freed.
 *   3. An indirect-block pointer update is written to disk BEFORE the
 *      block it points to is reused for another purpose.
 *
 * Dependency types modelled here (matching FreeBSD softdep.h):
 *
 *   SD_NEWBLK       — a newly allocated block
 *   SD_ALLOCDIRECT  — a di_db[]/di_ib[] pointer being set
 *   SD_ALLOCINDIR   — an indirect-block pointer being set
 *   SD_FREEBLKS     — blocks being freed (wait for inodedep to clear)
 *   SD_FREEFRAG     — a fragment being freed
 *   SD_INODEDEP     — an inode in flight
 *   SD_DIRADD       — a directory entry being added
 *   SD_DIRREM       — a directory entry being removed
 *   SD_MKDIR        — a mkdir in progress (holds both diradd + dotdot)
 *   SD_PAGEDEP      — a directory block with pending entries
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef SOFTDEP_H
#define SOFTDEP_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <sys/queue.h>

/* ------------------------------------------------------------------ */
/* Dependency kinds                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    SD_NEWBLK,
    SD_ALLOCDIRECT,
    SD_ALLOCINDIR,
    SD_FREEBLKS,
    SD_FREEFRAG,
    SD_INODEDEP,
    SD_DIRADD,
    SD_DIRREM,
    SD_MKDIR,
    SD_PAGEDEP,
} sd_kind_t;

/* ------------------------------------------------------------------ */
/* Dependency state flags                                               */
/* ------------------------------------------------------------------ */

#define SD_ATTACHED      0x0001  /* linked into a work list */
#define SD_COMPLETE      0x0002  /* all dependencies satisfied */
#define SD_ONWORKLIST    0x0004  /* on the pending-write worklist */
#define SD_DEPCOMPLETE   0x0008  /* this dep's own work is done */
#define SD_MKDIR_BODY    0x0010  /* mkdir body block written */
#define SD_MKDIR_PARENT  0x0020  /* mkdir parent direntry written */

/* ------------------------------------------------------------------ */
/* Base dependency node                                                 */
/* ------------------------------------------------------------------ */

struct sd_dep {
    TAILQ_ENTRY(sd_dep) sd_worklist; /* global worklist */
    LIST_ENTRY(sd_dep)  sd_hash;     /* hash by (block / inode) */

    sd_kind_t   sd_kind;
    uint32_t    sd_flags;

    /* Back-pointer list: deps that must complete AFTER this one */
    LIST_HEAD(, sd_dep) sd_dependents;
    LIST_ENTRY(sd_dep)  sd_dep_link; /* link in parent's sd_dependents */
    struct sd_dep      *sd_parent;   /* dep we are blocking */
};

TAILQ_HEAD(sd_worklist, sd_dep);

/* ------------------------------------------------------------------ */
/* Specific dependency structures                                       */
/* ------------------------------------------------------------------ */

/* A newly allocated block (must be written before any pointer to it) */
struct sd_newblk {
    struct sd_dep  nb_dep;
    int64_t        nb_blkno;
    uint32_t       nb_cgno;
};

/* An inode being modified (must be on disk before a diradd referencing it) */
struct sd_inodedep {
    struct sd_dep    id_dep;
    uint32_t         id_ino;
    uint32_t         id_nlinkdelta; /* pending nlink adjustments */
    /* Pending diradd list (added dir entries for this inode) */
    LIST_HEAD(, sd_dep) id_penddiradd;
    /* Saved "safe" inode copy (written to disk while real copy is pending) */
    uint8_t         *id_savedino;  /* malloc'd, sizeof ufs1/2_dinode */
};

/* A directory entry being added (must wait for inode dep to complete) */
struct sd_diradd {
    struct sd_dep    da_dep;
    uint32_t         da_ino;        /* inode being linked */
    uint32_t         da_dirino;     /* directory inode */
    off_t            da_offset;     /* byte offset in directory */
    uint16_t         da_reclen;
    uint8_t          da_type;
    uint8_t          da_namlen;
    char             da_name[256];
};

/* A directory entry being removed (must wait for inode dep to clear) */
struct sd_dirrem {
    struct sd_dep    dr_dep;
    uint32_t         dr_ino;        /* inode being unlinked */
    uint32_t         dr_dirino;     /* directory inode */
    off_t            dr_offset;     /* byte offset in directory */
};

/* A set of blocks being freed (must wait for all pointers to be removed) */
struct sd_freeblks {
    struct sd_dep    fb_dep;
    uint32_t         fb_ino;
    int64_t          fb_blknos[16]; /* up to 16 blocks per dep */
    int              fb_nblks;
    int              fb_bsize;
};

/* A mkdir in progress */
struct sd_mkdir {
    struct sd_dep    mk_dep;
    struct sd_diradd *mk_diradd;    /* entry in parent dir */
    struct sd_diradd *mk_dotdot;    /* ".." entry in new dir */
};

/* ------------------------------------------------------------------ */
/* Soft-updates mount state                                             */
/* ------------------------------------------------------------------ */

struct softdep_state {
    struct sd_worklist  ss_worklist;    /* pending work */
    pthread_mutex_t     ss_lock;

    /* Hash tables: inodedep by ino, pagedep by blkno */
    LIST_HEAD(, sd_dep) ss_inohash[256];
    LIST_HEAD(, sd_dep) ss_blkhash[512];

    uint32_t            ss_ndeps;       /* total live deps */
    uint32_t            ss_npending;    /* deps awaiting I/O */
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

struct ufs_mount;
struct inode;
struct buf;

/* Initialise soft-updates state for a mount. */
int  softdep_init(struct ufs_mount *ump);

/* Tear down: flush all remaining dependencies. */
void softdep_fini(struct ufs_mount *ump);

/* Flush all pending dependencies (called on fsync / umount). */
int  softdep_flush(struct ufs_mount *ump);

/*
 * Dependency tracking hooks.  These are called by the inode and
 * directory layers at the point where an in-memory structure is
 * modified.
 */

/* Record that inode ino has been allocated (must be on disk before diradd). */
void softdep_setup_inode_alloc(struct ufs_mount *ump, struct inode *ip,
                                uint16_t mode);

/* Record that a directory entry is being added. */
void softdep_setup_diradd(struct ufs_mount *ump,
                           struct inode *dp, struct inode *ip,
                           const char *name, size_t namelen,
                           off_t offset, uint8_t dtype);

/* Record that a directory entry is being removed. */
void softdep_setup_dirrem(struct ufs_mount *ump,
                           struct inode *dp, struct inode *ip,
                           off_t offset);

/* Record that blocks are being freed. */
void softdep_setup_freeblks(struct ufs_mount *ump, struct inode *ip,
                              int64_t *blknos, int nblks, int bsize);

/* Record completion of a mkdir (body + parent entry both written). */
void softdep_setup_mkdir(struct ufs_mount *ump,
                          struct inode *dp, struct inode *newip);

/* Called when a buffer with dependencies has been written to disk.
 * Processes all deps attached to that buffer. */
void softdep_buf_written(struct ufs_mount *ump, struct buf *bp);

/* Called on fsync of a file — flush all deps for that inode. */
int  softdep_fsync(struct ufs_mount *ump, struct inode *ip);

/* Return 1 if the write of inode ip must be deferred (dep not met). */
int  softdep_inode_defer(struct ufs_mount *ump, struct inode *ip);

#endif /* SOFTDEP_H */
