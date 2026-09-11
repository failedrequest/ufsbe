/*
 * fuse_ops.c — FUSE3 low-level operation callbacks.
 *
 * Maps FUSE low-level VFS operations to the ufs-fuse inode/dir/journal
 * layers.  All operations follow the pattern:
 *
 *   1. Acquire the relevant inode(s).
 *   2. Check permissions / preconditions.
 *   3. Perform the operation.
 *   4. Optionally journal the operation (if SUJ enabled).
 *   5. Register softdep dependency.
 *   6. Reply to FUSE.
 *   7. Release inode(s).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>

#include "fuse_ops.h"
#include "inode.h"
#include "dir.h"
#include "super.h"
#include "softdep.h"
#include "journal.h"
#include "block.h"
#include "ufs_mount.h"
#include "ufs_fs.h"
#include "ufs_dir.h"
#include "ufs_dinode.h"

/* ------------------------------------------------------------------ */
/* Global mount state (set once before event loop)                     */
/* ------------------------------------------------------------------ */

static struct ufs_mount *g_ump = NULL;

void
fuse_ops_set_mount(struct ufs_mount *ump)
{
    g_ump = ump;
}

/* ------------------------------------------------------------------ */
/* FUSE ↔ UFS inode number translation                                 */
/* FUSE uses ino=1 for root; UFS uses ROOTINO=2.                       */
/* ------------------------------------------------------------------ */

static inline uint32_t
fuse_ino_to_ufs(fuse_ino_t fi)
{
    return (fi == FUSE_ROOT_ID) ? ROOTINO : (uint32_t)fi;
}

static inline fuse_ino_t
ufs_ino_to_fuse(uint32_t ui)
{
    return (ui == ROOTINO) ? FUSE_ROOT_ID : (fuse_ino_t)ui;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static void
inode_to_fuse_entry(const struct ufs_mount *ump, const struct inode *ip,
                    struct fuse_entry_param *e)
{
    memset(e, 0, sizeof(*e));
    e->ino = ufs_ino_to_fuse(ip->i_number);
    e->generation = UFS_IS2(ump) ?
        (uint64_t)ip->i_din.di2.di_gen :
        (uint64_t)ip->i_din.di1.di_gen;
    inode_stat(ump, ip, &e->attr);
    e->attr_timeout  = 1.0;
    e->entry_timeout = 1.0;
}

/* Convert UFS inode mode bits to FUSE d_type */
static uint8_t
mode_to_dtype(uint16_t mode)
{
    switch (mode & IFMT) {
    case IFDIR:  return DT_DIR;
    case IFREG:  return DT_REG;
    case IFLNK:  return DT_LNK;
    case IFCHR:  return DT_CHR;
    case IFBLK:  return DT_BLK;
    case IFIFO:  return DT_FIFO;
    case IFSOCK: return DT_SOCK;
    default:     return DT_UNKNOWN;
    }
}

/* ------------------------------------------------------------------ */
/* init / destroy                                                       */
/* ------------------------------------------------------------------ */

static void
op_init(void *userdata, struct fuse_conn_info *conn)
{
    (void)userdata;
    (void)conn;
    /* Nothing extra to do; mount was already performed by main.c */
}

static void
op_destroy(void *userdata)
{
    (void)userdata;
    struct ufs_mount *ump = g_ump;
    if (ump == NULL)
        return;

    /* Flush soft-updates and journal */
    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_flush(ump);
    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_commit(ump);
        journal_fini(ump);
    }
    bdev_sync(ump->um_bdev);
    super_set_clean(ump, 1);
    inode_cache_fini(ump);
}

/* ------------------------------------------------------------------ */
/* lookup                                                               */
/* ------------------------------------------------------------------ */

static void
op_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct ufs_mount *ump = g_ump;
    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    uint32_t ino = 0;
    int rc = dir_lookup(ump, dp, name, strlen(name), &ino);
    inode_put(ump, dp);

    if (rc != 0) {
        fuse_reply_err(req, -rc);
        return;
    }

    struct inode *ip = inode_get(ump, ino);
    if (ip == NULL) {
        fuse_reply_err(req, EIO);
        return;
    }

    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, ip, &e);
    inode_put(ump, ip);
    fuse_reply_entry(req, &e);
}

/* ------------------------------------------------------------------ */
/* getattr                                                              */
/* ------------------------------------------------------------------ */

static void
op_getattr(fuse_req_t req, fuse_ino_t ino,
           struct fuse_file_info *fi)
{
    (void)fi;
    struct ufs_mount *ump = g_ump;
    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    struct stat st;
    inode_stat(ump, ip, &st);
    inode_put(ump, ip);
    fuse_reply_attr(req, &st, 1.0);
}

/* ------------------------------------------------------------------ */
/* setattr                                                              */
/* ------------------------------------------------------------------ */

static void
op_setattr(fuse_req_t req, fuse_ino_t ino, struct stat *attr,
           int to_set, struct fuse_file_info *fi)
{
    (void)fi;
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) {
        fuse_reply_err(req, EROFS);
        return;
    }

    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    if (to_set & FUSE_SET_ATTR_SIZE)
        inode_truncate(ump, ip, (uint64_t)attr->st_size);

    if (to_set & FUSE_SET_ATTR_MODE) {
        uint16_t mode = inode_mode(ump, ip);
        mode = (mode & IFMT) | (attr->st_mode & ~(unsigned)IFMT);
        if (UFS_IS2(ump))
            ip->i_din.di2.di_mode = mode;
        else
            ip->i_din.di1.di_mode = mode;
        ip->i_flag |= IN_MODIFIED | IN_CHANGE;
    }

    if (to_set & FUSE_SET_ATTR_UID) {
        if (UFS_IS2(ump)) ip->i_din.di2.di_uid = (uint32_t)attr->st_uid;
        else              ip->i_din.di1.di_uid = (uint32_t)attr->st_uid;
        ip->i_flag |= IN_MODIFIED | IN_CHANGE;
    }

    if (to_set & FUSE_SET_ATTR_GID) {
        if (UFS_IS2(ump)) ip->i_din.di2.di_gid = (uint32_t)attr->st_gid;
        else              ip->i_din.di1.di_gid = (uint32_t)attr->st_gid;
        ip->i_flag |= IN_MODIFIED | IN_CHANGE;
    }

    if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME)) {
        time_t atime = (to_set & FUSE_SET_ATTR_ATIME) ?
            attr->st_atime : (time_t)0;
        time_t mtime = (to_set & FUSE_SET_ATTR_MTIME) ?
            attr->st_mtime : (time_t)0;
        if (UFS_IS2(ump)) {
            if (atime) ip->i_din.di2.di_atime = atime;
            if (mtime) ip->i_din.di2.di_mtime = mtime;
        } else {
            if (atime) ip->i_din.di1.di_atime = (int32_t)atime;
            if (mtime) ip->i_din.di1.di_mtime = (int32_t)mtime;
        }
        ip->i_flag |= IN_MODIFIED;
    }

    if (UFS_JOURNAL(ump) && ump->um_journal)
        journal_write_inode(ump, ip);

    inode_update(ump, ip, 0);

    struct stat st;
    inode_stat(ump, ip, &st);
    inode_put(ump, ip);
    fuse_reply_attr(req, &st, 1.0);
}

/* ------------------------------------------------------------------ */
/* readlink                                                             */
/* ------------------------------------------------------------------ */

static void
op_readlink(fuse_req_t req, fuse_ino_t ino)
{
    struct ufs_mount *ump = g_ump;
    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    uint64_t sz = inode_size(ump, ip);
    if (sz == 0 || sz > 1023) {
        inode_put(ump, ip);
        fuse_reply_err(req, EINVAL);
        return;
    }

    char buf[1024];
    /* Short symlinks stored inline in block-pointer area */
    size_t maxinline = UFS_IS2(ump) ?
        (UFS_NDADDR + UFS_NIADDR) * sizeof(ufs2_daddr_t) :
        (UFS_NDADDR + UFS_NIADDR) * sizeof(ufs1_daddr_t);
    if (sz <= maxinline) {
        memcpy(buf, UFS_IS2(ump) ?
               (void *)ip->i_din.di2.di_db :
               (void *)ip->i_din.di1.di_db, (size_t)sz);
    } else {
        ssize_t n = inode_read(ump, ip, buf, (size_t)sz, 0);
        if (n < 0) {
            inode_put(ump, ip);
            fuse_reply_err(req, EIO);
            return;
        }
    }
    buf[sz] = '\0';
    inode_put(ump, ip);
    fuse_reply_readlink(req, buf);
}

/* ------------------------------------------------------------------ */
/* mknod / create                                                       */
/* ------------------------------------------------------------------ */

static void
op_mknod(fuse_req_t req, fuse_ino_t parent, const char *name,
         mode_t mode, dev_t rdev)
{
    (void)rdev;
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct inode *ip = inode_alloc(ump, (uint32_t)parent,
                                   (uint16_t)(mode & 0xFFFF),
                                   ctx->uid, ctx->gid);
    if (ip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    uint8_t dtype = mode_to_dtype((uint16_t)mode);
    int rc = dir_add_entry(ump, dp, name, strlen(name),
                           ip->i_number, dtype);
    if (rc != 0) {
        inode_unlink(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_setup_diradd(ump, dp, ip, name, strlen(name),
                              (off_t)(inode_size(ump, dp) - DIRBLKSIZ),
                              dtype);
    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_write_inode(ump, ip);
        journal_write_diradd(ump, ip->i_number, (uint32_t)parent,
                              (off_t)(inode_size(ump, dp) - DIRBLKSIZ),
                              dtype, name, strlen(name));
    }

    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, ip, &e);
    inode_put(ump, ip);
    inode_put(ump, dp);
    fuse_reply_entry(req, &e);
}

/* ------------------------------------------------------------------ */
/* mkdir                                                                */
/* ------------------------------------------------------------------ */

static void
op_mkdir(fuse_req_t req, fuse_ino_t parent, const char *name, mode_t mode)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    uint16_t dmode = (uint16_t)((mode & 07777) | IFDIR);
    struct inode *newip = inode_alloc(ump, (uint32_t)parent, dmode,
                                      ctx->uid, ctx->gid);
    if (newip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    /* nlink starts at 2 for directories ("." + parent entry) */
    inode_nlink_adj(ump, newip, 1);

    int rc = dir_init(ump, dp, newip);
    if (rc != 0) {
        inode_unlink(ump, newip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    rc = dir_add_entry(ump, dp, name, strlen(name),
                       newip->i_number, DT_DIR);
    if (rc != 0) {
        inode_unlink(ump, newip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    /* Parent gets +1 nlink for the ".." back-link */
    inode_nlink_adj(ump, dp, 1);

    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_setup_mkdir(ump, dp, newip);
    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_write_inode(ump, newip);
        journal_write_diradd(ump, newip->i_number, (uint32_t)parent,
                              (off_t)(inode_size(ump, dp) - DIRBLKSIZ),
                              DT_DIR, name, strlen(name));
    }

    inode_update(ump, dp, 0);

    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, newip, &e);
    inode_put(ump, newip);
    inode_put(ump, dp);
    fuse_reply_entry(req, &e);
}

/* ------------------------------------------------------------------ */
/* unlink                                                               */
/* ------------------------------------------------------------------ */

static void
op_unlink(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    uint32_t ino = 0;
    int rc = dir_lookup(ump, dp, name, strlen(name), &ino);
    if (rc != 0) {
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    struct inode *ip = inode_get(ump, ino);
    if (ip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, EIO);
        return;
    }

    off_t entry_off = 0; /* approximate; used only for journal */
    if (UFS_JOURNAL(ump) && ump->um_journal)
        journal_write_dirrem(ump, ino, (uint32_t)parent, entry_off);

    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_setup_dirrem(ump, dp, ip, entry_off);

    rc = dir_remove_entry(ump, dp, name, strlen(name));
    if (rc == 0)
        inode_unlink(ump, ip);   /* may free inode when nlink reaches 0 */
    else
        inode_put(ump, ip);

    inode_put(ump, dp);
    fuse_reply_err(req, (rc == 0) ? 0 : -rc);
}

/* ------------------------------------------------------------------ */
/* rmdir                                                                */
/* ------------------------------------------------------------------ */

static void
op_rmdir(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    uint32_t ino = 0;
    int rc = dir_lookup(ump, dp, name, strlen(name), &ino);
    if (rc != 0) {
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    struct inode *ip = inode_get(ump, ino);
    if (ip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, EIO);
        return;
    }

    if ((inode_mode(ump, ip) & IFMT) != IFDIR) {
        inode_put(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    if (!dir_isempty(ump, ip)) {
        inode_put(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, ENOTEMPTY);
        return;
    }

    if (UFS_JOURNAL(ump) && ump->um_journal)
        journal_write_dirrem(ump, ino, (uint32_t)parent, 0);

    rc = dir_remove_entry(ump, dp, name, strlen(name));
    if (rc == 0) {
        /* Adjust nlinks: dir has nlink=2 (. and parent ref), set to 0 */
        inode_nlink_adj(ump, ip, -2);
        inode_nlink_adj(ump, dp, -1);  /* ".." no longer points to parent */
        inode_unlink(ump, ip);
        inode_update(ump, dp, 0);
    } else {
        inode_put(ump, ip);
    }

    inode_put(ump, dp);
    fuse_reply_err(req, (rc == 0) ? 0 : -rc);
}

/* ------------------------------------------------------------------ */
/* symlink                                                              */
/* ------------------------------------------------------------------ */

static void
op_symlink(fuse_req_t req, const char *link, fuse_ino_t parent,
           const char *name)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct inode *ip = inode_alloc(ump, (uint32_t)parent,
                                   (uint16_t)(IFLNK | 0777),
                                   ctx->uid, ctx->gid);
    if (ip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    size_t llen = strlen(link);
    size_t maxinline = UFS_IS2(ump) ?
        (UFS_NDADDR + UFS_NIADDR) * sizeof(ufs2_daddr_t) :
        (UFS_NDADDR + UFS_NIADDR) * sizeof(ufs1_daddr_t);

    if (llen <= maxinline) {
        /* Store inline */
        if (UFS_IS2(ump))
            memcpy(ip->i_din.di2.di_db, link, llen);
        else
            memcpy(ip->i_din.di1.di_db, link, llen);
        if (UFS_IS2(ump))
            ip->i_din.di2.di_size = llen;
        else
            ip->i_din.di1.di_size = llen;
        ip->i_flag |= IN_MODIFIED;
    } else {
        inode_write(ump, ip, link, llen, 0);
    }

    int rc = dir_add_entry(ump, dp, name, strlen(name),
                           ip->i_number, DT_LNK);
    if (rc != 0) {
        inode_unlink(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_write_inode(ump, ip);
        journal_write_diradd(ump, ip->i_number, (uint32_t)parent, 0,
                              DT_LNK, name, strlen(name));
    }

    inode_update(ump, ip, 0);
    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, ip, &e);
    inode_put(ump, ip);
    inode_put(ump, dp);
    fuse_reply_entry(req, &e);
}

/* ------------------------------------------------------------------ */
/* link (hard link)                                                     */
/* ------------------------------------------------------------------ */

static void
op_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent,
        const char *newname)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(newparent));
    if (ip == NULL || dp == NULL) {
        if (ip) inode_put(ump, ip);
        if (dp) inode_put(ump, dp);
        fuse_reply_err(req, ENOENT);
        return;
    }

    uint8_t dtype = mode_to_dtype(inode_mode(ump, ip));
    int rc = dir_add_entry(ump, dp, newname, strlen(newname),
                           ip->i_number, dtype);
    if (rc != 0) {
        inode_put(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    inode_nlink_adj(ump, ip, 1);

    if (UFS_JOURNAL(ump) && ump->um_journal)
        journal_write_diradd(ump, (uint32_t)ino, (uint32_t)newparent, 0,
                              dtype, newname, strlen(newname));

    inode_update(ump, ip, 0);
    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, ip, &e);
    inode_put(ump, ip);
    inode_put(ump, dp);
    fuse_reply_entry(req, &e);
}

/* ------------------------------------------------------------------ */
/* rename                                                               */
/* ------------------------------------------------------------------ */

static void
op_rename(fuse_req_t req, fuse_ino_t parent, const char *name,
          fuse_ino_t newparent, const char *newname,
          unsigned int flags)
{
    (void)flags;
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *srdp = inode_get(ump, fuse_ino_to_ufs(parent));
    struct inode *dstdp = inode_get(ump, fuse_ino_to_ufs(newparent));
    if (srdp == NULL || dstdp == NULL) {
        if (srdp)  inode_put(ump, srdp);
        if (dstdp) inode_put(ump, dstdp);
        fuse_reply_err(req, ENOENT);
        return;
    }

    uint32_t src_ino = 0;
    int rc = dir_lookup(ump, srdp, name, strlen(name), &src_ino);
    if (rc != 0) {
        inode_put(ump, srdp);
        inode_put(ump, dstdp);
        fuse_reply_err(req, -rc);
        return;
    }

    struct inode *ip = inode_get(ump, src_ino);
    if (ip == NULL) {
        inode_put(ump, srdp);
        inode_put(ump, dstdp);
        fuse_reply_err(req, EIO);
        return;
    }

    /* Remove any existing destination entry */
    uint32_t dst_ino = 0;
    if (dir_lookup(ump, dstdp, newname, strlen(newname), &dst_ino) == 0) {
        struct inode *dstip = inode_get(ump, dst_ino);
        if (dstip != NULL) {
            dir_remove_entry(ump, dstdp, newname, strlen(newname));
            inode_unlink(ump, dstip);
        }
    }

    /* Add new name in dest dir, remove old name in src dir */
    uint8_t dtype = mode_to_dtype(inode_mode(ump, ip));
    dir_add_entry(ump, dstdp, newname, strlen(newname), src_ino, dtype);
    dir_remove_entry(ump, srdp, name, strlen(name));

    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_write_diradd(ump, src_ino, (uint32_t)newparent, 0,
                              dtype, newname, strlen(newname));
        journal_write_dirrem(ump, src_ino, (uint32_t)parent, 0);
    }

    inode_update(ump, ip, 0);
    inode_put(ump, ip);
    inode_put(ump, srdp);
    inode_put(ump, dstdp);
    fuse_reply_err(req, 0);
}

/* ------------------------------------------------------------------ */
/* open / release                                                       */
/* ------------------------------------------------------------------ */

static void
op_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct ufs_mount *ump = g_ump;
    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    /* Check write on read-only mount */
    if (UMP_RDONLY(ump) && (fi->flags & (O_WRONLY | O_RDWR))) {
        inode_put(ump, ip);
        fuse_reply_err(req, EROFS);
        return;
    }
    fi->fh = (uint64_t)fuse_ino_to_ufs(ino);
    inode_put(ump, ip);
    fuse_reply_open(req, fi);
}

static void
op_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

/* ------------------------------------------------------------------ */
/* read                                                                 */
/* ------------------------------------------------------------------ */

static void
op_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
        struct fuse_file_info *fi)
{
    (void)fi;
    struct ufs_mount *ump = g_ump;
    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    void *buf = malloc(size);
    if (buf == NULL) {
        inode_put(ump, ip);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    ssize_t n = inode_read(ump, ip, buf, size, off);
    inode_put(ump, ip);

    if (n < 0) {
        free(buf);
        fuse_reply_err(req, (int)-n);
        return;
    }

    fuse_reply_buf(req, buf, (size_t)n);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* write                                                                */
/* ------------------------------------------------------------------ */

static void
op_write(fuse_req_t req, fuse_ino_t ino, const char *buf, size_t size,
         off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    ssize_t n = inode_write(ump, ip, buf, size, off);

    if (UFS_JOURNAL(ump) && ump->um_journal && n > 0)
        journal_write_inode(ump, ip);

    inode_update(ump, ip, 0);
    inode_put(ump, ip);

    if (n < 0)
        fuse_reply_err(req, (int)-n);
    else
        fuse_reply_write(req, (size_t)n);
}

/* ------------------------------------------------------------------ */
/* fsync                                                                */
/* ------------------------------------------------------------------ */

static void
op_fsync(fuse_req_t req, fuse_ino_t ino, int datasync,
         struct fuse_file_info *fi)
{
    (void)datasync;
    (void)fi;
    struct ufs_mount *ump = g_ump;

    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, 0);
        return;
    }

    int rc = 0;
    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        rc = softdep_fsync(ump, ip);
    else
        rc = inode_update(ump, ip, 1);

    if (UFS_JOURNAL(ump) && ump->um_journal)
        journal_commit(ump);

    bdev_sync(ump->um_bdev);
    inode_put(ump, ip);
    fuse_reply_err(req, (rc == 0) ? 0 : -rc);
}

/* ------------------------------------------------------------------ */
/* opendir / releasedir                                                 */
/* ------------------------------------------------------------------ */

static void
op_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct ufs_mount *ump = g_ump;
    struct inode *ip = inode_get(ump, fuse_ino_to_ufs(ino));
    if (ip == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if ((inode_mode(ump, ip) & IFMT) != IFDIR) {
        inode_put(ump, ip);
        fuse_reply_err(req, ENOTDIR);
        return;
    }
    fi->fh = (uint64_t)fuse_ino_to_ufs(ino);
    inode_put(ump, ip);
    fuse_reply_open(req, fi);
}

static void
op_releasedir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    (void)ino;
    (void)fi;
    fuse_reply_err(req, 0);
}

/* ------------------------------------------------------------------ */
/* readdir                                                              */
/* ------------------------------------------------------------------ */

struct readdir_ctx {
    fuse_req_t  req;
    char       *buf;
    size_t      bufsize;
    size_t      bufused;
    struct ufs_mount *ump;
    int         error;
};

static int
readdir_cb(void *arg, uint32_t ino, uint8_t type, const char *name,
           size_t namelen, off_t next_off)
{
    struct readdir_ctx *ctx = arg;
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino  = (ino_t)ufs_ino_to_fuse(ino);
    st.st_mode = DTTOIF(type);

    char nbuf[MAXNAMLEN + 1];
    size_t nlen = (namelen > MAXNAMLEN) ? MAXNAMLEN : namelen;
    memcpy(nbuf, name, nlen);
    nbuf[nlen] = '\0';

    size_t entry_size = fuse_add_direntry(ctx->req, NULL, 0, nbuf,
                                          &st, next_off);
    if (ctx->bufused + entry_size > ctx->bufsize)
        return 1;  /* buffer full — stop */

    fuse_add_direntry(ctx->req,
                      ctx->buf + ctx->bufused,
                      ctx->bufsize - ctx->bufused,
                      nbuf, &st, next_off);
    ctx->bufused += entry_size;
    return 0;
}

static void
op_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
           struct fuse_file_info *fi)
{
    (void)fi;
    struct ufs_mount *ump = g_ump;

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(ino));
    if (dp == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    char *buf = malloc(size);
    if (buf == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, ENOMEM);
        return;
    }

    struct readdir_ctx ctx = {
        .req     = req,
        .buf     = buf,
        .bufsize = size,
        .bufused = 0,
        .ump     = ump,
        .error   = 0,
    };

    off_t pos = off;
    dir_readdir(ump, dp, &pos, readdir_cb, &ctx);
    inode_put(ump, dp);

    fuse_reply_buf(req, buf, ctx.bufused);
    free(buf);
}

/* ------------------------------------------------------------------ */
/* statfs                                                               */
/* ------------------------------------------------------------------ */

static void
op_statfs(fuse_req_t req, fuse_ino_t ino)
{
    (void)ino;
    struct ufs_mount *ump = g_ump;
    struct fs *fs = ump->um_fs;
    struct statvfs sv;
    memset(&sv, 0, sizeof(sv));

    sv.f_bsize   = (unsigned long)fs->fs_bsize;
    sv.f_frsize  = (unsigned long)fs->fs_fsize;
    sv.f_blocks  = (fsblkcnt_t)fs->fs_dsize;
    sv.f_bfree   = (fsblkcnt_t)(blkstofrags(fs, fs->fs_cstotal.cs_nbfree) +
                                  fs->fs_cstotal.cs_nffree);
    sv.f_bavail  = sv.f_bfree;
    sv.f_files   = (fsfilcnt_t)(fs->fs_ncg * fs->fs_ipg);
    sv.f_ffree   = (fsfilcnt_t)fs->fs_cstotal.cs_nifree;
    sv.f_favail  = sv.f_ffree;
    sv.f_namemax = MAXNAMLEN;

    fuse_reply_statfs(req, &sv);
}

/* ------------------------------------------------------------------ */
/* create (atomic open+mknod)                                          */
/* ------------------------------------------------------------------ */

static void
op_create(fuse_req_t req, fuse_ino_t parent, const char *name,
          mode_t mode, struct fuse_file_info *fi)
{
    struct ufs_mount *ump = g_ump;

    if (UMP_RDONLY(ump)) { fuse_reply_err(req, EROFS); return; }

    struct inode *dp = inode_get(ump, fuse_ino_to_ufs(parent));
    if (dp == NULL) { fuse_reply_err(req, ENOENT); return; }

    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    struct inode *ip = inode_alloc(ump, (uint32_t)parent,
                                   (uint16_t)((mode & 07777) | IFREG),
                                   ctx->uid, ctx->gid);
    if (ip == NULL) {
        inode_put(ump, dp);
        fuse_reply_err(req, ENOSPC);
        return;
    }

    int rc = dir_add_entry(ump, dp, name, strlen(name),
                           ip->i_number, DT_REG);
    if (rc != 0) {
        inode_unlink(ump, ip);
        inode_put(ump, dp);
        fuse_reply_err(req, -rc);
        return;
    }

    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_setup_diradd(ump, dp, ip, name, strlen(name), 0, DT_REG);
    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_write_inode(ump, ip);
        journal_write_diradd(ump, ip->i_number, (uint32_t)parent, 0,
                              DT_REG, name, strlen(name));
    }

    fi->fh = (uint64_t)ip->i_number;

    struct fuse_entry_param e;
    inode_to_fuse_entry(ump, ip, &e);
    inode_put(ump, ip);
    inode_put(ump, dp);
    fuse_reply_create(req, &e, fi);
}

/* ------------------------------------------------------------------ */
/* forget (FUSE reference counting)                                    */
/* ------------------------------------------------------------------ */

static void
op_forget(fuse_req_t req, fuse_ino_t ino, uint64_t nlookup)
{
    (void)ino;
    (void)nlookup;
    fuse_reply_none(req);
}

/* ------------------------------------------------------------------ */
/* Operation table                                                      */
/* ------------------------------------------------------------------ */

static const struct fuse_lowlevel_ops ufs_ops = {
    .init        = op_init,
    .destroy     = op_destroy,
    .lookup      = op_lookup,
    .forget      = op_forget,
    .getattr     = op_getattr,
    .setattr     = op_setattr,
    .readlink    = op_readlink,
    .mknod       = op_mknod,
    .mkdir       = op_mkdir,
    .unlink      = op_unlink,
    .rmdir       = op_rmdir,
    .symlink     = op_symlink,
    .link        = op_link,
    .rename      = op_rename,
    .open        = op_open,
    .read        = op_read,
    .write       = op_write,
    .release     = op_release,
    .opendir     = op_opendir,
    .readdir     = op_readdir,
    .releasedir  = op_releasedir,
    .fsync       = op_fsync,
    .statfs      = op_statfs,
    .create      = op_create,
};

const struct fuse_lowlevel_ops *
fuse_ufs_ops(void)
{
    return &ufs_ops;
}
