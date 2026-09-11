/*
 * dir.c — Directory entry operations.
 *
 * Directories in UFS are a linear sequence of DIRBLKSIZ-aligned
 * variable-length struct direct entries.  Each block is self-contained:
 * free space is represented by entries with d_ino == 0 or by a large
 * d_reclen on the last real entry in the block.
 *
 * This implementation scans blocks sequentially via inode_read/write,
 * which go through the buffer cache.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>

#include "dir.h"
#include "inode.h"
#include "block.h"
#include "ufs_fs.h"
#include "ufs_dir.h"
#include "ufs_mount.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

/*
 * Read one DIRBLKSIZ-aligned block of directory data into buf.
 * blkoff is the byte offset within the directory (must be DIRBLKSIZ-aligned).
 * Returns 0 on success.
 */
static int
dir_readblock(struct ufs_mount *ump, struct inode *dp,
              off_t blkoff_v, uint8_t *buf)
{
    ssize_t n = inode_read(ump, dp, buf, DIRBLKSIZ, blkoff_v);
    if (n < 0)
        return (int)n;
    if (n < DIRBLKSIZ)
        memset(buf + n, 0, (size_t)(DIRBLKSIZ - n));
    return 0;
}

static int
dir_writeblock(struct ufs_mount *ump, struct inode *dp,
               off_t blkoff_v, const uint8_t *buf)
{
    ssize_t n = inode_write(ump, dp, buf, DIRBLKSIZ, blkoff_v);
    if (n < 0)
        return (int)n;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Lookup                                                               */
/* ------------------------------------------------------------------ */

int
dir_lookup(struct ufs_mount *ump, struct inode *dp,
           const char *name, size_t namelen,
           uint32_t *ino_out)
{
    uint64_t dirsize = inode_size(ump, dp);
    uint8_t  buf[DIRBLKSIZ];

    for (off_t blk = 0; (uint64_t)blk < dirsize; blk += DIRBLKSIZ) {
        if (dir_readblock(ump, dp, blk, buf) != 0)
            return -EIO;

        const uint8_t *p   = buf;
        const uint8_t *end = buf + DIRBLKSIZ;

        while (p < end) {
            const struct direct *ep = (const struct direct *)p;
            if (ep->d_reclen == 0)
                break;
            if (ep->d_ino != 0 &&
                ep->d_namlen == (uint8_t)namelen &&
                memcmp(ep->d_name, name, namelen) == 0) {
                *ino_out = ep->d_ino;
                return 0;
            }
            p += ep->d_reclen;
        }
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Add entry                                                            */
/* ------------------------------------------------------------------ */

int
dir_add_entry(struct ufs_mount *ump, struct inode *dp,
              const char *name, size_t namelen,
              uint32_t ino, uint8_t type)
{
    if (namelen > MAXNAMLEN)
        return -ENAMETOOLONG;

    int      needed = DIRECTSIZ((int)namelen);
    uint64_t dirsize = inode_size(ump, dp);
    uint8_t  buf[DIRBLKSIZ];

    /* Pass 1: try to fit in an existing block */
    for (off_t blk = 0; (uint64_t)blk < dirsize; blk += DIRBLKSIZ) {
        if (dir_readblock(ump, dp, blk, buf) != 0)
            return -EIO;

        uint8_t *p   = buf;
        uint8_t *end = buf + DIRBLKSIZ;

        while (p < end) {
            struct direct *ep = (struct direct *)p;
            if (ep->d_reclen == 0)
                break;

            int epmin = (ep->d_ino == 0) ? 0 :
                        DIRECTSIZ(ep->d_namlen);
            int avail = (int)ep->d_reclen - epmin;

            if (avail >= needed) {
                if (ep->d_ino != 0) {
                    /* Split: shrink existing, append new */
                    uint16_t old_reclen = (uint16_t)epmin;
                    uint16_t new_reclen = (uint16_t)((int)ep->d_reclen -
                                                     epmin);
                    ep->d_reclen = old_reclen;
                    struct direct *np = (struct direct *)(p + old_reclen);
                    np->d_ino    = ino;
                    np->d_reclen = new_reclen;
                    np->d_type   = type;
                    np->d_namlen = (uint8_t)namelen;
                    memcpy(np->d_name, name, namelen);
                    np->d_name[namelen] = '\0';
                } else {
                    /* Reuse free slot */
                    ep->d_ino    = ino;
                    ep->d_type   = type;
                    ep->d_namlen = (uint8_t)namelen;
                    memcpy(ep->d_name, name, namelen);
                    ep->d_name[namelen] = '\0';
                }
                dp->i_flag |= IN_MODIFIED | IN_UPDATE | IN_CHANGE;
                return dir_writeblock(ump, dp, blk, buf);
            }
            p += ep->d_reclen;
        }
    }

    /* Pass 2: append a new DIRBLKSIZ block */
    memset(buf, 0, DIRBLKSIZ);
    struct direct *ep = (struct direct *)buf;
    ep->d_ino    = ino;
    ep->d_reclen = DIRBLKSIZ;
    ep->d_type   = type;
    ep->d_namlen = (uint8_t)namelen;
    memcpy(ep->d_name, name, namelen);
    ep->d_name[namelen] = '\0';

    off_t newblk = (off_t)dirsize;
    ssize_t n = inode_write(ump, dp, buf, DIRBLKSIZ, newblk);
    if (n < 0)
        return (int)n;
    dp->i_flag |= IN_MODIFIED | IN_UPDATE | IN_CHANGE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Remove entry                                                         */
/* ------------------------------------------------------------------ */

int
dir_remove_entry(struct ufs_mount *ump, struct inode *dp,
                 const char *name, size_t namelen)
{
    uint64_t dirsize = inode_size(ump, dp);
    uint8_t  buf[DIRBLKSIZ];

    for (off_t blk = 0; (uint64_t)blk < dirsize; blk += DIRBLKSIZ) {
        if (dir_readblock(ump, dp, blk, buf) != 0)
            return -EIO;

        uint8_t *p    = buf;
        uint8_t *end  = buf + DIRBLKSIZ;
        struct direct *prev = NULL;

        while (p < end) {
            struct direct *ep = (struct direct *)p;
            if (ep->d_reclen == 0)
                break;

            if (ep->d_ino != 0 &&
                ep->d_namlen == (uint8_t)namelen &&
                memcmp(ep->d_name, name, namelen) == 0) {
                /* Merge reclen into previous entry, or zero out */
                if (prev != NULL) {
                    prev->d_reclen = (uint16_t)(prev->d_reclen +
                                                ep->d_reclen);
                } else {
                    ep->d_ino = 0;
                }
                dp->i_flag |= IN_MODIFIED | IN_UPDATE | IN_CHANGE;
                return dir_writeblock(ump, dp, blk, buf);
            }
            prev = ep;
            p   += ep->d_reclen;
        }
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Initialise new directory                                             */
/* ------------------------------------------------------------------ */

int
dir_init(struct ufs_mount *ump, struct inode *dp, struct inode *newip)
{
    uint8_t buf[DIRBLKSIZ];
    memset(buf, 0, DIRBLKSIZ);

    struct dirtemplate *tmpl = (struct dirtemplate *)buf;

    /* "." entry */
    tmpl->dot_ino    = newip->i_number;
    tmpl->dot_reclen = DIRECTSIZ(1);
    tmpl->dot_type   = DT_DIR;
    tmpl->dot_namlen = 1;
    tmpl->dot_name[0] = '.';

    /* ".." entry */
    tmpl->dotdot_ino    = dp->i_number;
    tmpl->dotdot_reclen = (int16_t)(DIRBLKSIZ - DIRECTSIZ(1));
    tmpl->dotdot_type   = DT_DIR;
    tmpl->dotdot_namlen = 2;
    tmpl->dotdot_name[0] = '.';
    tmpl->dotdot_name[1] = '.';

    ssize_t n = inode_write(ump, newip, buf, DIRBLKSIZ, 0);
    if (n < 0)
        return (int)n;

    /* Update new dir size */
    if (UFS_IS2(ump))
        newip->i_din.di2.di_size = DIRBLKSIZ;
    else
        newip->i_din.di1.di_size = DIRBLKSIZ;

    newip->i_flag |= IN_MODIFIED | IN_UPDATE;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Empty check                                                           */
/* ------------------------------------------------------------------ */

int
dir_isempty(struct ufs_mount *ump, struct inode *dp)
{
    uint64_t dirsize = inode_size(ump, dp);
    uint8_t  buf[DIRBLKSIZ];

    for (off_t blk = 0; (uint64_t)blk < dirsize; blk += DIRBLKSIZ) {
        if (dir_readblock(ump, dp, blk, buf) != 0)
            return -EIO;

        const uint8_t *p   = buf;
        const uint8_t *end = buf + DIRBLKSIZ;

        while (p < end) {
            const struct direct *ep = (const struct direct *)p;
            if (ep->d_reclen == 0)
                break;
            if (ep->d_ino != 0) {
                if (ep->d_namlen == 1 && ep->d_name[0] == '.')
                    goto next;
                if (ep->d_namlen == 2 &&
                    ep->d_name[0] == '.' && ep->d_name[1] == '.')
                    goto next;
                return 0;  /* non-empty */
            }
next:
            p += ep->d_reclen;
        }
    }
    return 1;  /* empty */
}

/* ------------------------------------------------------------------ */
/* Readdir                                                              */
/* ------------------------------------------------------------------ */

int
dir_readdir(struct ufs_mount *ump, struct inode *dp,
            off_t *off, dir_readdir_cb cb, void *arg)
{
    uint64_t dirsize = inode_size(ump, dp);
    uint8_t  buf[DIRBLKSIZ];
    int      rc = 0;

    if ((uint64_t)*off >= dirsize)
        return 0;

    /* Align down to block boundary */
    off_t blkstart = (*off / DIRBLKSIZ) * DIRBLKSIZ;

    for (off_t blk = blkstart; (uint64_t)blk < dirsize; blk += DIRBLKSIZ) {
        if (dir_readblock(ump, dp, blk, buf) != 0)
            return -EIO;

        const uint8_t *p   = buf;
        const uint8_t *end = buf + DIRBLKSIZ;
        off_t          pos = blk;

        while (p < end) {
            const struct direct *ep = (const struct direct *)p;
            if (ep->d_reclen == 0)
                break;

            off_t entry_off  = pos;
            off_t next_off   = pos + ep->d_reclen;

            if (entry_off >= *off && ep->d_ino != 0) {
                rc = cb(arg, ep->d_ino, ep->d_type,
                        ep->d_name, ep->d_namlen, next_off);
                if (rc != 0) {
                    *off = next_off;
                    return rc;
                }
            }
            pos = next_off;
            p  += ep->d_reclen;
        }
    }

    *off = (off_t)dirsize;
    return 0;
}
