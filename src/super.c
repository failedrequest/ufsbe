/*
 * super.c — Superblock and cylinder-group management using libufs.
 *
 * Uses the FreeBSD libufs library (sbget/sbput/cgget/cgput) so that the
 * superblock and CG are read/written correctly regardless of UFS1/UFS2
 * version, byte order, or field layout.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

#include <sys/param.h>
#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <libufs.h>

#include "super.h"
#include "block.h"
#include "ufs_fs.h"
#include "ufs_mount.h"

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

int
bitmap_isset(const uint8_t *map, int bit)
{
    return (map[bit / 8] >> (bit % 8)) & 1;
}

void
bitmap_set(uint8_t *map, int bit)
{
    map[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

void
bitmap_clr(uint8_t *map, int bit)
{
    map[bit / 8] &= (uint8_t)~(1 << (bit % 8));
}

/* ------------------------------------------------------------------ */
/* Superblock read / write                                              */
/* ------------------------------------------------------------------ */

int
super_read(struct ufs_mount *ump)
{
    struct fs *fs = NULL;
    int flags = UFS_NOHASHFAIL | UFS_NOMSG;

    /* sbget allocates the fs struct and reads the csum area */
    int rc = sbget(ump->um_bdev->bd_fd, &fs, UFS_STDSB, flags);
    if (rc != 0) {
        /* Try again at alternate locations */
        for (int i = 0; ufs_sb_offsets[i] >= 0; i++) {
            rc = sbget(ump->um_bdev->bd_fd, &fs,
                       ufs_sb_offsets[i], flags);
            if (rc == 0)
                break;
        }
        if (rc != 0)
            return -EINVAL;
    }

    ump->um_fs       = fs;
    ump->um_sboffset = fs->fs_sblockloc;

    /* Set UFS2 flag */
    if (fs->fs_magic == FS_UFS2_MAGIC)
        ump->um_flags |= UFS_MOUNT_UFS2;

    /* Detect unclean mount */
    if (!fs->fs_clean || (fs->fs_flags & FS_UNCLEAN))
        ump->um_flags |= UFS_MOUNT_UNCLEAN;

    /* Detect SUJ */
    if (fs->fs_flags & FS_SUJ)
        ump->um_flags |= UFS_MOUNT_JOURNAL;

    /* Detect soft updates */
    if (fs->fs_flags & FS_DOSOFTDEP)
        ump->um_flags |= UFS_MOUNT_SOFTDEP;

    /* Detect B-epsilon tree mode */
    if (fs->fs_flags & FS_BETREE)
        ump->um_flags |= UFS_MOUNT_BETREE;

    /* Derived constants */
    ump->um_nindir   = (uint64_t)fs->fs_nindir;
    ump->um_bptrtodb = (uint64_t)fs->fs_fsbtodb;
    ump->um_seqinc   = (uint64_t)fs->fs_frag;

    /* sbget reads and sets up fs->fs_csp internally */
    ump->um_csmem = NULL; /* owned by fs struct from sbget */

    /* Mark in-use */
    if (!UMP_RDONLY(ump)) {
        fs->fs_clean = FS_ISDIRTY;
        fs->fs_flags |= FS_UNCLEAN;
        super_write(ump);
    }

    return 0;
}

int
super_write(struct ufs_mount *ump)
{
    struct fs *fs = ump->um_fs;
    fs->fs_time = (ufs_time_t)time(NULL);

    /* sbput writes the superblock and all alternate copies */
    int rc = sbput(ump->um_bdev->bd_fd, fs, 0);
    return (rc == 0) ? 0 : -EIO;
}

void
super_dirty(struct ufs_mount *ump)
{
    ump->um_fs->fs_fmod = 1;
}

int
super_flush(struct ufs_mount *ump)
{
    if (ump->um_fs->fs_fmod) {
        ump->um_fs->fs_fmod = 0;
        return super_write(ump);
    }
    return 0;
}

int
super_set_clean(struct ufs_mount *ump, int clean)
{
    struct fs *fs = ump->um_fs;
    fs->fs_clean = clean ? FS_ISCLEAN : FS_ISDIRTY;
    if (clean)
        fs->fs_flags &= ~FS_UNCLEAN;
    else
        fs->fs_flags |= FS_UNCLEAN;
    return super_write(ump);
}

/* ------------------------------------------------------------------ */
/* Cylinder-group read / write via libufs cgget/cgput                  */
/* ------------------------------------------------------------------ */

int
cg_read(struct ufs_mount *ump, uint32_t cgno, struct cg *cgbuf)
{
    if (cgno >= ump->um_fs->fs_ncg)
        return -EINVAL;
    int rc = cgget(ump->um_bdev->bd_fd, ump->um_fs, (int)cgno, cgbuf);
    return (rc == 0) ? 0 : -EIO;
}

int
cg_write(struct ufs_mount *ump, uint32_t cgno, const struct cg *cgbuf)
{
    if (cgno >= ump->um_fs->fs_ncg)
        return -EINVAL;
    /* cgput takes a non-const pointer */
    int rc = cgput(ump->um_bdev->bd_fd, ump->um_fs, (struct cg *)cgbuf);
    return (rc == 0) ? 0 : -EIO;
}

/* ------------------------------------------------------------------ */
/* Block allocation within a CG                                        */
/* ------------------------------------------------------------------ */

int64_t
cg_alloc_block(struct ufs_mount *ump, uint32_t cg,
               uint32_t pref, int size)
{
    struct fs *fs    = ump->um_fs;
    size_t     cgsz  = (size_t)fs->fs_cgsize;
    struct cg *cgp   = malloc(cgsz);
    if (cgp == NULL)
        return -1;

    if (cg_read(ump, cg, cgp) != 0) {
        free(cgp);
        return -1;
    }

    uint8_t   *blkmap  = cg_blksfree(cgp);
    int        nblks   = (int)(cgp->cg_ndblk / fs->fs_frag);
    int        start   = (int)(pref ? (pref - (ufs2_daddr_t)cgbase(fs, cg)) /
                                       fs->fs_frag : cgp->cg_rotor);
    int64_t    result  = -1;

    for (int i = 0; i < nblks; i++) {
        int blk = (start + i) % nblks;
        int fragbase = blk * fs->fs_frag;
        int allfree  = 1;
        for (int f = 0; f < fs->fs_frag; f++) {
            if (!bitmap_isset(blkmap, fragbase + f)) {
                allfree = 0;
                break;
            }
        }
        if (!allfree)
            continue;

        for (int f = 0; f < fs->fs_frag; f++)
            bitmap_clr(blkmap, fragbase + f);

        cgp->cg_cs.cs_nbfree--;
        cgp->cg_rotor = (blk + 1) % nblks;
        result = (int64_t)cgbase(fs, cg) + fragbase;

        /* Update global summary */
        fs->fs_cstotal.cs_nbfree--;
        if (fs->fs_csp)
            fs->fs_csp[cg].cs_nbfree--;
        super_dirty(ump);

        cg_write(ump, cg, cgp);
        break;
    }

    free(cgp);
    (void)size;
    return result;
}

void
cg_free_block(struct ufs_mount *ump, uint32_t cg, int64_t bno, int size)
{
    struct fs  *fs   = ump->um_fs;
    size_t      cgsz = (size_t)fs->fs_cgsize;
    struct cg  *cgp  = malloc(cgsz);
    if (cgp == NULL)
        return;

    if (cg_read(ump, cg, cgp) != 0) {
        free(cgp);
        return;
    }

    uint8_t *blkmap = cg_blksfree(cgp);
    int64_t  base   = (int64_t)cgbase(fs, cg);
    int      frag   = (int)(bno - base);
    int      nfrags = (size > 0) ? (size / fs->fs_fsize) : fs->fs_frag;

    for (int f = 0; f < nfrags; f++)
        bitmap_set(blkmap, frag + f);

    if (nfrags < fs->fs_frag) {
        cgp->cg_cs.cs_nffree += nfrags;
        cgp->cg_frsum[nfrags]++;
        fs->fs_cstotal.cs_nffree += nfrags;
        if (fs->fs_csp)
            fs->fs_csp[cg].cs_nffree += nfrags;
    } else {
        cgp->cg_cs.cs_nbfree++;
        fs->fs_cstotal.cs_nbfree++;
        if (fs->fs_csp)
            fs->fs_csp[cg].cs_nbfree++;
    }

    super_dirty(ump);
    cg_write(ump, cg, cgp);
    free(cgp);
}

int64_t
cg_alloc_frags(struct ufs_mount *ump, uint32_t cg, int64_t pref, int frags)
{
    struct fs *fs    = ump->um_fs;
    size_t     cgsz  = (size_t)fs->fs_cgsize;
    struct cg *cgp   = malloc(cgsz);
    if (cgp == NULL)
        return -1;

    if (cg_read(ump, cg, cgp) != 0) {
        free(cgp);
        return -1;
    }

    uint8_t *blkmap = cg_blksfree(cgp);
    int      nfrags_total = (int)cgp->cg_ndblk;
    int64_t  base   = (int64_t)cgbase(fs, cg);
    int      start  = (pref > base) ? (int)(pref - base) : 0;
    int64_t  result = -1;

    for (int i = 0; i <= nfrags_total - frags; i++) {
        int pos = (start + i) % (nfrags_total - frags + 1);
        int ok  = 1;
        for (int f = 0; f < frags; f++) {
            if (!bitmap_isset(blkmap, pos + f)) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            continue;

        for (int f = 0; f < frags; f++)
            bitmap_clr(blkmap, pos + f);

        cgp->cg_cs.cs_nffree -= frags;
        cgp->cg_frsum[frags]--;
        fs->fs_cstotal.cs_nffree -= frags;
        if (fs->fs_csp)
            fs->fs_csp[cg].cs_nffree -= frags;
        result = base + pos;

        super_dirty(ump);
        cg_write(ump, cg, cgp);
        break;
    }

    free(cgp);
    return result;
}

/* ------------------------------------------------------------------ */
/* Inode allocation within a CG                                        */
/* ------------------------------------------------------------------ */

uint32_t
cg_alloc_inode(struct ufs_mount *ump, uint32_t cg, int isdir)
{
    struct fs *fs    = ump->um_fs;
    size_t     cgsz  = (size_t)fs->fs_cgsize;
    struct cg *cgp   = malloc(cgsz);
    if (cgp == NULL)
        return 0;

    if (cg_read(ump, cg, cgp) != 0) {
        free(cgp);
        return 0;
    }

    uint8_t  *imap  = cg_inosused(cgp);
    uint32_t  nipg  = fs->fs_ipg;
    uint32_t  ino   = 0;

    for (uint32_t i = 0; i < nipg; i++) {
        if (!bitmap_isset(imap, (int)i)) {
            bitmap_set(imap, (int)i);
            cgp->cg_cs.cs_nifree--;
            fs->fs_cstotal.cs_nifree--;
            if (fs->fs_csp)
                fs->fs_csp[cg].cs_nifree--;
            if (isdir) {
                cgp->cg_cs.cs_ndir++;
                fs->fs_cstotal.cs_ndir++;
                if (fs->fs_csp)
                    fs->fs_csp[cg].cs_ndir++;
            }
            ino = cg * nipg + i;
            cgp->cg_irotor = (int32_t)i;
            super_dirty(ump);
            cg_write(ump, cg, cgp);
            break;
        }
    }

    free(cgp);
    return ino;
}

void
cg_free_inode(struct ufs_mount *ump, uint32_t cg, uint32_t ino, int isdir)
{
    struct fs *fs    = ump->um_fs;
    size_t     cgsz  = (size_t)fs->fs_cgsize;
    struct cg *cgp   = malloc(cgsz);
    if (cgp == NULL)
        return;

    if (cg_read(ump, cg, cgp) != 0) {
        free(cgp);
        return;
    }

    uint8_t *imap  = cg_inosused(cgp);
    int      ioff  = (int)(ino % fs->fs_ipg);

    bitmap_clr(imap, ioff);
    cgp->cg_cs.cs_nifree++;
    fs->fs_cstotal.cs_nifree++;
    if (fs->fs_csp)
        fs->fs_csp[cg].cs_nifree++;
    if (isdir) {
        cgp->cg_cs.cs_ndir--;
        fs->fs_cstotal.cs_ndir--;
        if (fs->fs_csp)
            fs->fs_csp[cg].cs_ndir--;
    }

    super_dirty(ump);
    cg_write(ump, cg, cgp);
    free(cgp);
}

/* ------------------------------------------------------------------ */
/* Filesystem-wide allocation                                          */
/* ------------------------------------------------------------------ */

int64_t
fs_alloc_block(struct ufs_mount *ump, int64_t pref, int size)
{
    struct fs *fs     = ump->um_fs;
    uint32_t   startcg = (pref > 0) ? (uint32_t)dtog(fs, pref) : 0;

    for (uint32_t i = 0; i < fs->fs_ncg; i++) {
        uint32_t cg = (startcg + i) % fs->fs_ncg;
        if (fs->fs_csp && fs->fs_csp[cg].cs_nbfree == 0)
            continue;
        int64_t blkno = cg_alloc_block(ump, cg,
                                        (uint32_t)(pref > 0 ? pref : 0),
                                        size);
        if (blkno >= 0)
            return blkno;
    }
    return -1;
}

uint32_t
fs_alloc_inode(struct ufs_mount *ump, uint32_t parent_ino, int isdir)
{
    struct fs *fs     = ump->um_fs;
    uint32_t   startcg = (uint32_t)ino_to_cg(fs, parent_ino);

    for (uint32_t i = 0; i < fs->fs_ncg; i++) {
        uint32_t cg = (startcg + i) % fs->fs_ncg;
        if (fs->fs_csp && fs->fs_csp[cg].cs_nifree == 0)
            continue;
        uint32_t ino = cg_alloc_inode(ump, cg, isdir);
        if (ino != 0)
            return ino;
    }
    return 0;
}

void
fs_free_block(struct ufs_mount *ump, int64_t bno, int size)
{
    struct fs *fs = ump->um_fs;
    uint32_t   cg = (uint32_t)dtog(fs, bno);
    cg_free_block(ump, cg, bno, size);
}

void
fs_free_inode(struct ufs_mount *ump, uint32_t ino, int isdir)
{
    struct fs *fs = ump->um_fs;
    uint32_t   cg = (uint32_t)ino_to_cg(fs, ino);
    cg_free_inode(ump, cg, ino, isdir);
}
