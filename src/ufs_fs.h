/*
 * ufs_fs.h — Thin wrapper around the system UFS on-disk format headers.
 *
 * We use the FreeBSD system headers directly so that the struct fs,
 * struct cg, and all macros match exactly what newfs writes on disk.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * FS_BETREE (0x00000800) — filesystem was formatted by newfs_ufsbe.
 * When set, per-inode block maps are stored as B-epsilon trees rooted
 * in di_extb[0]; directory indexes are B-epsilon trees rooted in
 * di_extb[1].  di_db[] and di_ib[] are all zero.  fsck_ufsbe understands
 * this layout; the standard fsck_ffs must NOT be run on such a filesystem.
 * Value 0x00000800 is the next free bit after FS_TRIM (0x00000400) and
 * is within the FS_SUPPORTED (0x00FFFFFF) range so the kernel will not
 * clear it at mount time.
 */

#ifndef UFS_FS_H
#define UFS_FS_H

#include <sys/types.h>
#include <sys/param.h>
#include <ufs/ufs/dinode.h>   /* must come before fs.h for type defs */
#include <ufs/ffs/fs.h>

/* ------------------------------------------------------------------ */
/* Extra constants not in the system header                             */
/* ------------------------------------------------------------------ */

/* fs_clean values */
#define FS_ISCLEAN  1
#define FS_ISDIRTY  0

/*
 * FS_BETREE — B-epsilon tree block map and directory index.
 * Must be within FS_SUPPORTED (0x00FFFFFF) so the kernel retains it.
 */
#ifndef FS_BETREE
#define FS_BETREE   0x00000800
#endif

/* SUJ journal parameters (SUJ_MIN is already defined in system fs.h) */
#define SUJ_MAX_SIZE    (32 * 1024 * 1024)

/* SBSIZE may not be defined in newer system headers */
#ifndef SBSIZE
#define SBSIZE 8192
#endif

/* DEV_BSIZE may not be visible here without sys/param.h */
#ifndef DEV_BSIZE
#define DEV_BSIZE 512
#endif

#ifndef MAXFRAG
#define MAXFRAG 8
#endif

/* ------------------------------------------------------------------ */
/* Superblock search list (already defined in system header as         */
/* SBLOCKSEARCH, but we also keep our own name)                        */
/* ------------------------------------------------------------------ */

static const int64_t ufs_sb_offsets[] = {
    SBLOCK_UFS2,
    SBLOCK_UFS1,
    SBLOCK_FLOPPY,
    SBLOCK_PIGGY,
    -1
};

#endif /* UFS_FS_H */
