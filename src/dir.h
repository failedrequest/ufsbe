/*
 * dir.h — Directory layer interface.
 *
 * Provides:
 *  - Directory entry lookup (name → inode number)
 *  - Directory entry addition (for create/link/mkdir)
 *  - Directory entry removal (for unlink/rmdir)
 *  - Directory read (for readdir / getdents)
 *  - Directory creation (initialise "." and ".." entries)
 *  - Empty-directory check (for rmdir)
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DIR_H
#define DIR_H

#include <stdint.h>
#include <stddef.h>
#include "ufs_mount.h"
#include "ufs_inode.h"
#include "ufs_dir.h"

/* ------------------------------------------------------------------ */
/* Lookup                                                               */
/* ------------------------------------------------------------------ */

/* Look up name (len bytes, not NUL-terminated) in directory dp.
 * On success stores the found inode number in *ino_out and returns 0.
 * Returns -ENOENT if not found, negative errno on I/O error. */
int dir_lookup(struct ufs_mount *ump, struct inode *dp,
               const char *name, size_t namelen,
               uint32_t *ino_out);

/* ------------------------------------------------------------------ */
/* Add / remove entries                                                 */
/* ------------------------------------------------------------------ */

/* Add a directory entry (name → ino) to directory dp.
 * type is one of the DT_* constants.
 * Returns 0 on success, negative errno on failure. */
int dir_add_entry(struct ufs_mount *ump, struct inode *dp,
                  const char *name, size_t namelen,
                  uint32_t ino, uint8_t type);

/* Remove the entry with the given name from directory dp.
 * Returns 0 on success, -ENOENT if not found. */
int dir_remove_entry(struct ufs_mount *ump, struct inode *dp,
                     const char *name, size_t namelen);

/* ------------------------------------------------------------------ */
/* Create and populate a new directory ("." and "..") */
/* ------------------------------------------------------------------ */

/* Initialise a new (empty) directory inode with "." and ".." entries.
 * dp is the parent directory, newip is the newly-allocated directory.
 * Returns 0 on success. */
int dir_init(struct ufs_mount *ump, struct inode *dp,
             struct inode *newip);

/* ------------------------------------------------------------------ */
/* Check for empty directory                                            */
/* ------------------------------------------------------------------ */

/* Returns 1 if dp contains only "." and ".." entries (or none),
 * 0 if it has real entries, negative errno on error. */
int dir_isempty(struct ufs_mount *ump, struct inode *dp);

/* ------------------------------------------------------------------ */
/* Read entries (for readdir)                                           */
/* ------------------------------------------------------------------ */

/* Cookie type passed back by dir_readdir and used on the next call. */
typedef off_t dir_cookie_t;

/* Read up to maxentries directory entries starting at byte offset *off.
 * Callback is invoked for each entry; returning non-zero from callback
 * stops the scan and dir_readdir returns the callback's return value.
 * *off is updated to the offset of the next unread entry on return.
 *
 * Callback args: (void *arg, uint32_t ino, uint8_t type,
 *                  const char *name, size_t namelen, off_t next_off)
 */
typedef int (*dir_readdir_cb)(void *arg, uint32_t ino, uint8_t type,
                               const char *name, size_t namelen,
                               off_t next_off);

int dir_readdir(struct ufs_mount *ump, struct inode *dp,
                off_t *off, dir_readdir_cb cb, void *arg);

#endif /* DIR_H */
