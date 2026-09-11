/*
 * inode.h — Inode layer interface.
 *
 * Provides:
 *  - In-core inode cache (get/put/reclaim)
 *  - On-disk inode read/write
 *  - Block-map (bmap): logical block → physical FS block number
 *  - Block allocation for a file
 *  - File truncation
 *  - Inode creation and reclamation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef INODE_H
#define INODE_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/stat.h>

#include "ufs_mount.h"
#include "ufs_inode.h"

/* ------------------------------------------------------------------ */
/* Inode cache lifecycle                                                */
/* ------------------------------------------------------------------ */

/* Initialise the inode cache for the given mount. */
int  inode_cache_init(struct ufs_mount *ump);

/* Tear down the inode cache, writing all dirty inodes. */
void inode_cache_fini(struct ufs_mount *ump);

/* Get an inode by number: looks up cache, reads from disk on miss.
 * Increments i_refcnt.  Caller must call inode_put() when done. */
struct inode *inode_get(struct ufs_mount *ump, uint32_t ino);

/* Decrement ref count; marks for reclaim when it reaches zero. */
void inode_put(struct ufs_mount *ump, struct inode *ip);

/* Write the in-core inode back to disk. */
int  inode_update(struct ufs_mount *ump, struct inode *ip, int sync);

/* ------------------------------------------------------------------ */
/* Inode create / reclaim                                               */
/* ------------------------------------------------------------------ */

/* Allocate a new inode (mode determines type). */
struct inode *inode_alloc(struct ufs_mount *ump, uint32_t parent_ino,
                          uint16_t mode, uint32_t uid, uint32_t gid);

/* Drop nlink and reclaim (unlink) an inode when nlink reaches 0. */
int  inode_unlink(struct ufs_mount *ump, struct inode *ip);

/* ------------------------------------------------------------------ */
/* Block mapping and I/O                                                */
/* ------------------------------------------------------------------ */

/* Map logical block number lbn to a physical FS block number.
 * If alloc != 0, allocate missing blocks.
 * Returns FS block number, 0 for a hole, or -1 on error. */
int64_t inode_bmap(struct ufs_mount *ump, struct inode *ip,
                   int64_t lbn, int alloc);

/* Read bytes from a file into buf.
 * off and size are in bytes; returns bytes read or -errno. */
ssize_t inode_read(struct ufs_mount *ump, struct inode *ip,
                   void *buf, size_t size, off_t off);

/* Write bytes from buf into a file.
 * Allocates blocks as needed; returns bytes written or -errno. */
ssize_t inode_write(struct ufs_mount *ump, struct inode *ip,
                    const void *buf, size_t size, off_t off);

/* Truncate a file to newsize bytes (may extend with a hole). */
int  inode_truncate(struct ufs_mount *ump, struct inode *ip,
                    uint64_t newsize);

/* ------------------------------------------------------------------ */
/* Attribute helpers                                                    */
/* ------------------------------------------------------------------ */

/* Fill a struct stat from an in-core inode. */
void inode_stat(const struct ufs_mount *ump, const struct inode *ip,
                struct stat *st);

/* Set timestamps from in-core flags (IN_ACCESS / IN_UPDATE / IN_CHANGE). */
void inode_update_times(struct inode *ip, int flags);

/* Return the file size. */
uint64_t inode_size(const struct ufs_mount *ump, const struct inode *ip);

/* Return the inode's mode. */
uint16_t inode_mode(const struct ufs_mount *ump, const struct inode *ip);

/* Return the nlink count. */
int16_t  inode_nlink(const struct ufs_mount *ump, const struct inode *ip);

/* Modify nlink by delta, mark dirty. */
void inode_nlink_adj(struct ufs_mount *ump, struct inode *ip, int delta);

#endif /* INODE_H */
