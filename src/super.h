/*
 * super.h — Superblock and cylinder-group management interface.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef SUPER_H
#define SUPER_H

#include <stdint.h>
#include "ufs_mount.h"
#include "ufs_fs.h"

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                      */
/* ------------------------------------------------------------------ */

/* Read and validate the superblock; populate ump->um_fs.
 * Also reads the cylinder-group summary area. */
int super_read(struct ufs_mount *ump);

/* Write the superblock back to disk. */
int super_write(struct ufs_mount *ump);

/* Mark superblock dirty (deferred write). */
void super_dirty(struct ufs_mount *ump);

/* Flush superblock if dirty. */
int super_flush(struct ufs_mount *ump);

/* Mark filesystem clean/dirty in superblock (also updates fs_time). */
int super_set_clean(struct ufs_mount *ump, int clean);

/* ------------------------------------------------------------------ */
/* Cylinder-group operations                                            */
/* ------------------------------------------------------------------ */

/* Read a cylinder group from disk into a caller-supplied buffer.
 * *cgbuf must be at least fs->fs_cgsize bytes. */
int cg_read(struct ufs_mount *ump, uint32_t cgno, struct cg *cgbuf);

/* Write a cylinder group back to disk. */
int cg_write(struct ufs_mount *ump, uint32_t cgno, const struct cg *cgbuf);

/* ------------------------------------------------------------------ */
/* Block allocation in a cylinder group                                 */
/* ------------------------------------------------------------------ */

/* Allocate a full block (all frags) in the given cg.
 * Returns the FS block number, or -1 on failure. */
int64_t cg_alloc_block(struct ufs_mount *ump, uint32_t cg,
                        uint32_t pref, int size);

/* Free a block (or fragment) in the given cg. */
void cg_free_block(struct ufs_mount *ump, uint32_t cg, int64_t bno,
                   int size);

/* Allocate contiguous fragments of size frags in cg near pref. */
int64_t cg_alloc_frags(struct ufs_mount *ump, uint32_t cg,
                        int64_t pref, int frags);

/* ------------------------------------------------------------------ */
/* Inode allocation in a cylinder group                                 */
/* ------------------------------------------------------------------ */

/* Allocate an inode in cg.  Returns inode number or 0 on failure. */
uint32_t cg_alloc_inode(struct ufs_mount *ump, uint32_t cg, int isdir);

/* Free an inode in cg. */
void cg_free_inode(struct ufs_mount *ump, uint32_t cg, uint32_t ino,
                   int isdir);

/* ------------------------------------------------------------------ */
/* Filesystem-wide allocation (wraps cg_alloc_*) */
/* ------------------------------------------------------------------ */

/* Allocate a block near preferred block pref. */
int64_t fs_alloc_block(struct ufs_mount *ump, int64_t pref, int size);

/* Allocate an inode (preferring cg of parent directory). */
uint32_t fs_alloc_inode(struct ufs_mount *ump, uint32_t parent_ino,
                        int isdir);

/* Free a block. */
void fs_free_block(struct ufs_mount *ump, int64_t bno, int size);

/* Free an inode. */
void fs_free_inode(struct ufs_mount *ump, uint32_t ino, int isdir);

/* ------------------------------------------------------------------ */
/* Bitmap helpers (exposed for softdep use)                             */
/* ------------------------------------------------------------------ */

int  bitmap_isset(const uint8_t *map, int bit);
void bitmap_set(uint8_t *map, int bit);
void bitmap_clr(uint8_t *map, int bit);

#endif /* SUPER_H */
