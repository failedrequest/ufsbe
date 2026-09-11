/*
 * inode.c — Inode layer: cache, disk I/O, bmap, read/write, truncate.
 *
 * Block mapping strategy (matches FreeBSD ufs_bmap):
 *   Direct blocks:         di_db[0..11]   (lbn 0..11)
 *   Single indirect:       di_ib[0]       (lbn 12..12+nindir-1)
 *   Double indirect:       di_ib[1]
 *   Triple indirect:       di_ib[2]
 *
 * Both UFS1 (32-bit block pointers) and UFS2 (64-bit) are supported.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <time.h>
#include <stdio.h>
#include <sys/stat.h>

#include "inode.h"
#include "super.h"
#include "block.h"
#include "ufs_fs.h"
#include "ufs_dinode.h"
#include "ufs_mount.h"
#include "betree.h"

/* ------------------------------------------------------------------ */
/* Inode cache helpers                                                  */
/* ------------------------------------------------------------------ */

int
inode_cache_init(struct ufs_mount *ump)
{
    for (int i = 0; i < INODE_HASH_SIZE; i++)
        LIST_INIT(&ump->um_ihash[i]);
    TAILQ_INIT(&ump->um_ilru);
    ump->um_ninode = 0;
    pthread_mutex_init(&ump->um_ilock, NULL);
    return 0;
}

void
inode_cache_fini(struct ufs_mount *ump)
{
    struct inode *ip, *tmp;

    pthread_mutex_lock(&ump->um_ilock);
    TAILQ_FOREACH_SAFE(ip, &ump->um_ilru, i_lru, tmp) {
        if (ip->i_flag & IN_MODIFIED)
            inode_update(ump, ip, 1);
        TAILQ_REMOVE(&ump->um_ilru, ip, i_lru);
        LIST_REMOVE(ip, i_hash);
        pthread_rwlock_destroy(&ip->i_lock);
        free(ip);
    }
    pthread_mutex_unlock(&ump->um_ilock);
    pthread_mutex_destroy(&ump->um_ilock);
}

/* ------------------------------------------------------------------ */
/* Low-level on-disk inode read / write                                */
/* ------------------------------------------------------------------ */

static int
dinode_read(struct ufs_mount *ump, uint32_t ino, struct inode *ip)
{
    struct fs *fs   = ump->um_fs;
    int64_t    blkno = ino_to_fsba(fs, ino);
    int        off   = (int)ino_to_fsbo(fs, ino);
    uint32_t   bsize = (uint32_t)fs->fs_bsize;

    struct buf *bp = buf_bread(ump->um_bdev, blkno, bsize);
    if (bp == NULL)
        return -EIO;

    if (UFS_IS2(ump)) {
        struct ufs2_dinode *dip =
            (struct ufs2_dinode *)bp->b_data + off;
        ip->i_din.di2 = *dip;
        /* Restore B-ε roots from di_extb[] (unused by kernel for EA here) */
        ip->i_be_blkmap_root = (int64_t)dip->di_extb[0];
        ip->i_be_dir_root    = (int64_t)dip->di_extb[1];
    } else {
        struct ufs1_dinode *dip =
            (struct ufs1_dinode *)bp->b_data + off;
        ip->i_din.di1 = *dip;
        /* UFS1: no spare 64-bit slots — roots are in-core only */
        ip->i_be_blkmap_root = 0;
        ip->i_be_dir_root    = 0;
    }

    brelse(ump->um_bdev, bp);
    return 0;
}

int
inode_update(struct ufs_mount *ump, struct inode *ip, int sync)
{
    struct fs *fs    = ump->um_fs;
    int64_t    blkno = ino_to_fsba(fs, ip->i_number);
    int        off   = (int)ino_to_fsbo(fs, ip->i_number);
    uint32_t   bsize = (uint32_t)fs->fs_bsize;

    struct buf *bp = buf_bread(ump->um_bdev, blkno, bsize);
    if (bp == NULL)
        return -EIO;

    if (UFS_IS2(ump)) {
        struct ufs2_dinode *dip =
            (struct ufs2_dinode *)bp->b_data + off;
        *dip = ip->i_din.di2;
        /* Persist B-ε roots in di_extb[] */
        dip->di_extb[0] = (ufs2_daddr_t)ip->i_be_blkmap_root;
        dip->di_extb[1] = (ufs2_daddr_t)ip->i_be_dir_root;
    } else {
        struct ufs1_dinode *dip =
            (struct ufs1_dinode *)bp->b_data + off;
        *dip = ip->i_din.di1;
        /* UFS1: roots not persisted on-disk */
    }

    ip->i_flag &= ~IN_MODIFIED;

    if (sync)
        return buf_bwrite(ump->um_bdev, bp);

    bdwrite(bp);
    brelse(ump->um_bdev, bp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Inode cache: get / put                                               */
/* ------------------------------------------------------------------ */

struct inode *
inode_get(struct ufs_mount *ump, uint32_t ino)
{
    struct inode *ip;
    uint32_t h = INODE_HASH(ump, ino);

    pthread_mutex_lock(&ump->um_ilock);
    LIST_FOREACH(ip, &ump->um_ihash[h], i_hash) {
        if (ip->i_number == ino) {
            ip->i_refcnt++;
            /* Move to MRU head */
            TAILQ_REMOVE(&ump->um_ilru, ip, i_lru);
            TAILQ_INSERT_HEAD(&ump->um_ilru, ip, i_lru);
            pthread_mutex_unlock(&ump->um_ilock);
            return ip;
        }
    }
    pthread_mutex_unlock(&ump->um_ilock);

    /* Cache miss — allocate and read from disk */
    ip = calloc(1, sizeof(*ip));
    if (ip == NULL)
        return NULL;

    ip->i_ump    = ump;
    ip->i_number = ino;
    ip->i_refcnt = 1;
    ip->i_flag   = IN_HASHED;
    pthread_rwlock_init(&ip->i_lock, NULL);

    if (dinode_read(ump, ino, ip) != 0) {
        free(ip);
        return NULL;
    }

    ip->i_effnlink = UFS_IS2(ump) ?
        ip->i_din.di2.di_nlink : ip->i_din.di1.di_nlink;

    pthread_mutex_lock(&ump->um_ilock);
    /* Check for a race — another thread may have loaded it */
    struct inode *ip2;
    LIST_FOREACH(ip2, &ump->um_ihash[h], i_hash) {
        if (ip2->i_number == ino) {
            ip2->i_refcnt++;
            pthread_mutex_unlock(&ump->um_ilock);
            pthread_rwlock_destroy(&ip->i_lock);
            free(ip);
            return ip2;
        }
    }
    LIST_INSERT_HEAD(&ump->um_ihash[h], ip, i_hash);
    TAILQ_INSERT_HEAD(&ump->um_ilru, ip, i_lru);
    ump->um_ninode++;
    pthread_mutex_unlock(&ump->um_ilock);

    return ip;
}

void
inode_put(struct ufs_mount *ump, struct inode *ip)
{
    pthread_mutex_lock(&ump->um_ilock);
    if (ip->i_refcnt > 0)
        ip->i_refcnt--;
    pthread_mutex_unlock(&ump->um_ilock);

    /* Write back dirty inode asynchronously */
    if (ip->i_flag & IN_MODIFIED)
        inode_update(ump, ip, 0);
}

/* ------------------------------------------------------------------ */
/* Block mapping (bmap) — backed by B-ε tree                           */
/*                                                                      */
/* The B-ε tree maps int64_t logical-block-number → int64_t fs-block.  */
/* On the first allocation for an inode the tree is created (root=0    */
/* until betree_insert sets bt_root).  The root blkno is kept in       */
/* ip->i_be_blkmap_root and flushed to di_extb[0] by inode_update.    */
/* ------------------------------------------------------------------ */

int64_t
inode_bmap(struct ufs_mount *ump, struct inode *ip,
           int64_t lbn, int alloc)
{
    struct betree bt;
    betree_init(&bt, ump, ip->i_be_blkmap_root);

    int64_t pbn = 0;
    int rc = betree_lookup(&bt, lbn, &pbn);

    if (rc == 0)
        return pbn;   /* found */

    if (rc == -ENOENT && !alloc)
        return 0;     /* hole */

    if (rc != -ENOENT)
        return -1;    /* I/O error */

    /* Allocate a new data block and record it in the tree */
    struct fs *fs = ump->um_fs;
    int64_t newpbn = fs_alloc_block(ump, 0, fs->fs_bsize);
    if (newpbn < 0)
        return -1;

    rc = betree_insert(&bt, lbn, newpbn);
    if (rc != 0) {
        fs_free_block(ump, newpbn, fs->fs_bsize);
        return -1;
    }

    /* Persist the (possibly new) root */
    if (bt.bt_root != ip->i_be_blkmap_root) {
        ip->i_be_blkmap_root = bt.bt_root;
        ip->i_flag |= IN_MODIFIED;
    }

    return newpbn;
}

/* ------------------------------------------------------------------ */
/* File read / write                                                    */
/* ------------------------------------------------------------------ */

ssize_t
inode_read(struct ufs_mount *ump, struct inode *ip,
           void *buf, size_t size, off_t off)
{
    struct fs *fs    = ump->um_fs;
    uint64_t   fsize = inode_size(ump, ip);
    uint8_t   *dst   = buf;
    ssize_t    nread = 0;

    if ((uint64_t)off >= fsize)
        return 0;
    if ((uint64_t)off + size > fsize)
        size = (size_t)(fsize - (uint64_t)off);

    while (size > 0) {
        int64_t lbn    = lblkno(fs, off);
        int64_t blkoff = blkoff(fs, off);
        int64_t blksz  = (int64_t)fs->fs_bsize;
        int64_t tocopy = blksz - blkoff;
        if ((size_t)tocopy > size)
            tocopy = (int64_t)size;

        int64_t pbn = inode_bmap(ump, ip, lbn, 0);
        if (pbn <= 0) {
            /* Hole: return zeroes */
            memset(dst, 0, (size_t)tocopy);
        } else {
            struct buf *bp = buf_bread(ump->um_bdev, pbn,
                                   (uint32_t)fs->fs_bsize);
            if (bp == NULL)
                return (nread > 0) ? nread : -EIO;
            memcpy(dst, bp->b_data + blkoff, (size_t)tocopy);
            brelse(ump->um_bdev, bp);
        }

        dst    += tocopy;
        off    += tocopy;
        size   -= (size_t)tocopy;
        nread  += tocopy;
    }

    /* Update access time */
    ip->i_flag |= IN_ACCESS;

    return nread;
}

ssize_t
inode_write(struct ufs_mount *ump, struct inode *ip,
            const void *buf, size_t size, off_t off)
{
    struct fs  *fs    = ump->um_fs;
    const uint8_t *src = buf;
    ssize_t     nwrit  = 0;

    while (size > 0) {
        int64_t lbn    = lblkno(fs, off);
        int64_t blkoff_v = blkoff(fs, off);
        int64_t blksz  = (int64_t)fs->fs_bsize;
        int64_t tocopy = blksz - blkoff_v;
        if ((size_t)tocopy > size)
            tocopy = (int64_t)size;

        int64_t pbn = inode_bmap(ump, ip, lbn, 1);
        if (pbn < 0)
            return (nwrit > 0) ? nwrit : -ENOSPC;

        struct buf *bp;
        if (tocopy == blksz) {
            /* Full-block overwrite: use getblk to avoid a read */
            bp = getblk(ump->um_bdev, pbn, (uint32_t)fs->fs_bsize);
        } else {
            bp = buf_bread(ump->um_bdev, pbn, (uint32_t)fs->fs_bsize);
        }
        if (bp == NULL)
            return (nwrit > 0) ? nwrit : -EIO;

        memcpy(bp->b_data + blkoff_v, src, (size_t)tocopy);
        bdwrite(bp);
        brelse(ump->um_bdev, bp);

        src   += tocopy;
        off   += tocopy;
        size  -= (size_t)tocopy;
        nwrit += tocopy;
    }

    /* Update file size if we extended it */
    uint64_t newoff = (uint64_t)off;
    if (newoff > inode_size(ump, ip)) {
        if (UFS_IS2(ump))
            ip->i_din.di2.di_size = newoff;
        else
            ip->i_din.di1.di_size = newoff;
        ip->i_flag |= IN_SIZEMOD;
    }
    ip->i_flag |= IN_UPDATE | IN_MODIFIED;

    return nwrit;
}

/* ------------------------------------------------------------------ */
/* Truncation helpers — B-ε tree backed                                */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Key-collect context and callback for partial truncation              */
/* ------------------------------------------------------------------ */

struct key_collect_ctx {
    int64_t *keys;
    int      n;
    int      cap;
};

static int
key_collect_cb(void *arg, int64_t key, int64_t value)
{
    struct key_collect_ctx *c = arg;
    (void)value;
    if (c->n < c->cap)
        c->keys[c->n++] = key;
    return 0;
}

/* Callback for betree_range: free each mapped data block. */
struct free_range_ctx {
    struct ufs_mount *ump;
    int               bsize;
};

static int
free_block_cb(void *arg, int64_t key, int64_t value)
{
    struct free_range_ctx *ctx = arg;
    (void)key;
    fs_free_block(ctx->ump, value, ctx->bsize);
    return 0;
}

static void
inode_free_blocks_from(struct ufs_mount *ump, struct inode *ip,
                       int64_t lbn_start)
{
    if (ip->i_be_blkmap_root == 0)
        return;

    struct fs *fs = ump->um_fs;
    struct betree bt;
    betree_init(&bt, ump, ip->i_be_blkmap_root);

    /* Free the data blocks for all lbn >= lbn_start */
    struct free_range_ctx fctx = { ump, fs->fs_bsize };
    betree_range(&bt, lbn_start, INT64_MAX, free_block_cb, &fctx);

    /* Truncating to zero: destroy entire blkmap tree */
    if (lbn_start == 0) {
        betree_destroy(&bt);
        ip->i_be_blkmap_root = 0;
        ip->i_flag |= IN_MODIFIED;
        return;
    }

    /* Partial truncation: collect keys >= lbn_start then delete them */
    int cap = 4096;
    int64_t *keys = malloc((size_t)cap * sizeof(int64_t));
    if (keys == NULL) return;

    struct key_collect_ctx kctx = { keys, 0, cap };
    betree_range(&bt, lbn_start, INT64_MAX, key_collect_cb, &kctx);

    for (int i = 0; i < kctx.n; i++)
        betree_delete(&bt, keys[i]);

    free(keys);

    if (bt.bt_root != ip->i_be_blkmap_root) {
        ip->i_be_blkmap_root = bt.bt_root;
        ip->i_flag |= IN_MODIFIED;
    }
}

int
inode_truncate(struct ufs_mount *ump, struct inode *ip, uint64_t newsize)
{
    struct fs *fs    = ump->um_fs;
    uint64_t   oldsize = inode_size(ump, ip);

    if (newsize == oldsize)
        return 0;

    if (newsize > oldsize) {
        /* Extending: just update size (holes will be zeroed on read) */
        if (UFS_IS2(ump))
            ip->i_din.di2.di_size = newsize;
        else
            ip->i_din.di1.di_size = newsize;
        ip->i_flag |= IN_MODIFIED | IN_UPDATE;
        return 0;
    }

    /* Shrinking: free blocks beyond newsize */
    int64_t lbn_first_free = (int64_t)lblkno(fs, newsize);
    if (blkoff(fs, (off_t)newsize) != 0)
        lbn_first_free++;  /* partial last block kept */

    inode_free_blocks_from(ump, ip, lbn_first_free);

    if (UFS_IS2(ump))
        ip->i_din.di2.di_size = newsize;
    else
        ip->i_din.di1.di_size = newsize;

    ip->i_flag |= IN_MODIFIED | IN_UPDATE | IN_CHANGE;
    return inode_update(ump, ip, 0);
}

/* ------------------------------------------------------------------ */
/* Inode allocation / reclaim                                           */
/* ------------------------------------------------------------------ */

struct inode *
inode_alloc(struct ufs_mount *ump, uint32_t parent_ino,
            uint16_t mode, uint32_t uid, uint32_t gid)
{
    int isdir = ((mode & IFMT) == IFDIR);
    uint32_t ino = fs_alloc_inode(ump, parent_ino, isdir);
    if (ino == 0)
        return NULL;

    struct inode *ip = calloc(1, sizeof(*ip));
    if (ip == NULL) {
        fs_free_inode(ump, ino, isdir);
        return NULL;
    }

    ip->i_ump    = ump;
    ip->i_number = ino;
    ip->i_refcnt = 1;
    ip->i_flag   = IN_HASHED | IN_MODIFIED;
    pthread_rwlock_init(&ip->i_lock, NULL);

    time_t now = time(NULL);
    if (UFS_IS2(ump)) {
        ip->i_din.di2.di_mode  = mode;
        ip->i_din.di2.di_uid   = uid;
        ip->i_din.di2.di_gid   = gid;
        ip->i_din.di2.di_nlink = 1;
        ip->i_din.di2.di_atime = now;
        ip->i_din.di2.di_mtime = now;
        ip->i_din.di2.di_ctime = now;
        ip->i_din.di2.di_birthtime = now;
        ip->i_din.di2.di_gen   = (int32_t)now;
    } else {
        ip->i_din.di1.di_mode  = mode;
        ip->i_din.di1.di_uid   = uid;
        ip->i_din.di1.di_gid   = gid;
        ip->i_din.di1.di_nlink = 1;
        ip->i_din.di1.di_atime = (int32_t)now;
        ip->i_din.di1.di_mtime = (int32_t)now;
        ip->i_din.di1.di_ctime = (int32_t)now;
        ip->i_din.di1.di_gen   = (int32_t)now;
    }
    ip->i_effnlink = 1;

    /* Insert into cache */
    uint32_t h = INODE_HASH(ump, ino);
    pthread_mutex_lock(&ump->um_ilock);
    LIST_INSERT_HEAD(&ump->um_ihash[h], ip, i_hash);
    TAILQ_INSERT_HEAD(&ump->um_ilru, ip, i_lru);
    ump->um_ninode++;
    pthread_mutex_unlock(&ump->um_ilock);

    inode_update(ump, ip, 1);
    return ip;
}

int
inode_unlink(struct ufs_mount *ump, struct inode *ip)
{
    int isdir = ((inode_mode(ump, ip) & IFMT) == IFDIR);

    inode_nlink_adj(ump, ip, -1);
    if (inode_nlink(ump, ip) == 0) {
        /* Truncate to zero */
        inode_truncate(ump, ip, 0);
        inode_update(ump, ip, 1);
        fs_free_inode(ump, ip->i_number, isdir);
        /* Remove from cache */
        pthread_mutex_lock(&ump->um_ilock);
        LIST_REMOVE(ip, i_hash);
        TAILQ_REMOVE(&ump->um_ilru, ip, i_lru);
        ump->um_ninode--;
        pthread_mutex_unlock(&ump->um_ilock);
        pthread_rwlock_destroy(&ip->i_lock);
        free(ip);
        return 0;
    }
    return inode_update(ump, ip, 0);
}

/* ------------------------------------------------------------------ */
/* Attribute helpers                                                    */
/* ------------------------------------------------------------------ */

void
inode_stat(const struct ufs_mount *ump, const struct inode *ip,
           struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = ip->i_number;

    if (UFS_IS2(ump)) {
        const struct ufs2_dinode *d = &ip->i_din.di2;
        st->st_mode  = d->di_mode;
        st->st_uid   = d->di_uid;
        st->st_gid   = d->di_gid;
        st->st_nlink = d->di_nlink;
        st->st_size  = (off_t)d->di_size;
        st->st_blksize = (d->di_blksize > 0) ? (blksize_t)d->di_blksize :
                         (blksize_t)ump->um_fs->fs_bsize;
        st->st_blocks = (blkcnt_t)d->di_blocks;
        st->st_atime  = (time_t)d->di_atime;
        st->st_mtime  = (time_t)d->di_mtime;
        st->st_ctime  = (time_t)d->di_ctime;
    } else {
        const struct ufs1_dinode *d = &ip->i_din.di1;
        st->st_mode  = d->di_mode;
        st->st_uid   = d->di_uid;
        st->st_gid   = d->di_gid;
        st->st_nlink = d->di_nlink;
        st->st_size  = (off_t)d->di_size;
        st->st_blksize = (blksize_t)ump->um_fs->fs_bsize;
        st->st_blocks = (blkcnt_t)d->di_blocks;
        st->st_atime  = (time_t)d->di_atime;
        st->st_mtime  = (time_t)d->di_mtime;
        st->st_ctime  = (time_t)d->di_ctime;
    }
}

void
inode_update_times(struct inode *ip, int flags)
{
    time_t now = time(NULL);
    if (ip->i_ump && UFS_IS2(ip->i_ump)) {
        if (flags & IN_ACCESS)  ip->i_din.di2.di_atime = now;
        if (flags & IN_UPDATE)  ip->i_din.di2.di_mtime = now;
        if (flags & IN_CHANGE)  ip->i_din.di2.di_ctime = now;
    } else {
        if (flags & IN_ACCESS)  ip->i_din.di1.di_atime = (int32_t)now;
        if (flags & IN_UPDATE)  ip->i_din.di1.di_mtime = (int32_t)now;
        if (flags & IN_CHANGE)  ip->i_din.di1.di_ctime = (int32_t)now;
    }
    ip->i_flag &= ~(IN_ACCESS | IN_UPDATE | IN_CHANGE);
    ip->i_flag |= IN_MODIFIED;
}

uint64_t
inode_size(const struct ufs_mount *ump, const struct inode *ip)
{
    return UFS_IS2(ump) ? ip->i_din.di2.di_size : ip->i_din.di1.di_size;
}

uint16_t
inode_mode(const struct ufs_mount *ump, const struct inode *ip)
{
    return UFS_IS2(ump) ? ip->i_din.di2.di_mode : ip->i_din.di1.di_mode;
}

int16_t
inode_nlink(const struct ufs_mount *ump, const struct inode *ip)
{
    return UFS_IS2(ump) ? ip->i_din.di2.di_nlink : ip->i_din.di1.di_nlink;
}

void
inode_nlink_adj(struct ufs_mount *ump, struct inode *ip, int delta)
{
    if (UFS_IS2(ump))
        ip->i_din.di2.di_nlink = (int16_t)(ip->i_din.di2.di_nlink + delta);
    else
        ip->i_din.di1.di_nlink = (int16_t)(ip->i_din.di1.di_nlink + delta);
    ip->i_effnlink += delta;
    ip->i_flag |= IN_MODIFIED | IN_CHANGE;
}

