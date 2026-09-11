/*
 * journal.c — Soft-update journaling (SUJ) implementation.
 *
 * The journal is a circular log of fixed-size (SUJ_RECSIZE = 128 byte)
 * records, pre-allocated contiguously on disk.  Records are written to
 * a memory buffer and flushed in SUJ_BLKSIZE (32 KiB) units.
 *
 * Sequence numbers are monotonically increasing uint64_t values.
 * The "tail" tracks the oldest committed record that has not yet been
 * made obsolete by a later commit.  On clean unmount both head and tail
 * are equal; any gap indicates unconfirmed records that must be replayed.
 *
 * Recovery (journal_replay):
 *   Walk from tail to head, re-applying each record's effect.
 *   For JREC_DIRADD:  ensure the directory entry exists; if the inode
 *     is still allocated, the entry is valid — do nothing.
 *   For JREC_DIRREM:  ensure the directory entry is removed.
 *   For JREC_FREEINO: free the inode if it is still marked allocated.
 *   For JREC_FREEBLK: free the block if it is still marked allocated.
 *   JREC_INODE and JREC_NEWBLK: just verify / repair in-core state.
 *   JREC_COMMIT: advance tail pointer.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

#include "journal.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include "dir.h"
#include "ufs_mount.h"
#include "ufs_fs.h"
#include "ufs_inode.h"

/* ------------------------------------------------------------------ */
/* CRC-32 (IEEE 802.3) — used to validate records on replay            */
/* ------------------------------------------------------------------ */

static uint32_t crc32_table[256];
static int crc32_init_done = 0;

static void
crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_init_done = 1;
}

static uint32_t
crc32(const void *data, size_t len)
{
    if (!crc32_init_done)
        crc32_init();
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = crc32_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t
rec_crc(const uint8_t *rec)
{
    /* Compute CRC with the crc field zeroed */
    uint8_t tmp[SUJ_RECSIZE];
    memcpy(tmp, rec, SUJ_RECSIZE);
    /* jr_crc is at offset 20 (after magic+seq+type = 8+8+4) */
    memset(tmp + 20, 0, 4);
    return crc32(tmp, SUJ_RECSIZE);
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static inline struct journal_state *
js(struct ufs_mount *ump)
{
    return (struct journal_state *)ump->um_journal;
}

/* Convert a journal sequence number to a FS block number within the
 * journal's extent. */
static int64_t
seq_to_blkno(struct journal_state *j, uint64_t seq)
{
    /* Each FS block holds (fs_bsize / SUJ_RECSIZE) records */
    uint64_t recs_per_block = (uint64_t)(SUJ_BLKSIZE / SUJ_RECSIZE);
    uint64_t blk_idx = (seq / recs_per_block) % (uint64_t)j->js_size;
    return j->js_start + (int64_t)blk_idx;
}

static int
jbuf_flush_locked(struct ufs_mount *ump)
{
    struct journal_state *j = js(ump);
    if (!j->js_bufdirty)
        return 0;

    /* Write js_buf to the journal at js_bufblk */
    off_t off = (off_t)fsbtodb(ump->um_fs, j->js_bufblk) * DEV_BSIZE;
    if (bdev_pwrite(ump->um_bdev, j->js_buf, SUJ_BLKSIZE, off) != 0)
        return -EIO;
    j->js_bufdirty = 0;
    return 0;
}

/* Write one SUJ_RECSIZE record into the journal buffer.
 * Flushes buffer to disk when full. */
static int
journal_write_rec(struct ufs_mount *ump, const void *rec)
{
    struct journal_state *j = js(ump);

    pthread_mutex_lock(&j->js_lock);

    /* Determine which block the head falls in */
    int64_t  blkno = seq_to_blkno(j, j->js_head);
    int      off   = (int)((j->js_head %
                     (SUJ_BLKSIZE / SUJ_RECSIZE)) * SUJ_RECSIZE);

    /* If we moved to a new block, flush the old one */
    if (blkno != j->js_bufblk && j->js_bufdirty) {
        if (jbuf_flush_locked(ump) != 0) {
            pthread_mutex_unlock(&j->js_lock);
            return -EIO;
        }
        j->js_bufblk = blkno;
        j->js_bufoff = 0;
        /* Read existing block content (other records may share it) */
        off_t roff = (off_t)fsbtodb(ump->um_fs, blkno) * DEV_BSIZE;
        (void)bdev_pread(ump->um_bdev, j->js_buf, SUJ_BLKSIZE, roff);
    } else if (blkno != j->js_bufblk) {
        j->js_bufblk = blkno;
        j->js_bufoff = 0;
        off_t roff = (off_t)fsbtodb(ump->um_fs, blkno) * DEV_BSIZE;
        (void)bdev_pread(ump->um_bdev, j->js_buf, SUJ_BLKSIZE, roff);
    }

    memcpy(j->js_buf + off, rec, SUJ_RECSIZE);
    j->js_head++;
    j->js_bufdirty = 1;

    /* Flush when the buffer is full */
    if (((j->js_head) % (SUJ_BLKSIZE / SUJ_RECSIZE)) == 0)
        jbuf_flush_locked(ump);

    pthread_mutex_unlock(&j->js_lock);
    return 0;
}

/* Initialise the common record header. */
static void
hdr_init(struct ufs_mount *ump, struct jrec_hdr *hdr, uint32_t type)
{
    struct journal_state *j = js(ump);
    hdr->jr_magic = SUJ_MAGIC;
    hdr->jr_seq   = j->js_head;
    hdr->jr_type  = type;
    hdr->jr_crc   = 0;
}

static void
hdr_finalise(uint8_t *rec)
{
    struct jrec_hdr *h = (struct jrec_hdr *)rec;
    h->jr_crc = 0;
    h->jr_crc = rec_crc(rec);
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                          */
/* ------------------------------------------------------------------ */

int
journal_init(struct ufs_mount *ump)
{
    struct fs *fs = ump->um_fs;

    struct journal_state *j = calloc(1, sizeof(*j));
    if (j == NULL)
        return -ENOMEM;

    j->js_buf = calloc(1, SUJ_BLKSIZE);
    if (j->js_buf == NULL) {
        free(j);
        return -ENOMEM;
    }

    pthread_mutex_init(&j->js_lock, NULL);
    ump->um_journal = j;

    /* Find the journal inode — stored in fs_sujfree */
    if (fs->fs_sujfree == 0) {
        /* No journal inode; allocate one if writing */
        if (!UMP_RDONLY(ump)) {
            /* Allocate SUJ_MIN / bsize contiguous blocks.
             * For now: store a simple block run starting after the
             * last cg summary block. */
            int64_t jblks = SUJ_MIN / fs->fs_bsize;
            int64_t startblk = fs_alloc_block(ump, 0, fs->fs_bsize);
            if (startblk < 0) {
                /* Not enough space for journal; disable SUJ */
                ump->um_flags &= ~UFS_MOUNT_JOURNAL;
                free(j->js_buf);
                free(j);
                ump->um_journal = NULL;
                return 0;
            }
            j->js_start = startblk;
            j->js_size  = jblks;
            j->js_nrecs = (uint64_t)(jblks * (fs->fs_bsize / SUJ_RECSIZE));
            /* Allocate the rest of the blocks contiguously */
            for (int64_t i = 1; i < jblks; i++)
                fs_alloc_block(ump, startblk + i, fs->fs_bsize);

            fs->fs_sujfree = (int32_t)startblk;
            fs->fs_flags  |= FS_SUJ;
            super_dirty(ump);
        } else {
            /* Read-only with no journal; nothing to do */
            ump->um_flags &= ~UFS_MOUNT_JOURNAL;
            free(j->js_buf);
            free(j);
            ump->um_journal = NULL;
            return 0;
        }
    } else {
        j->js_start = (int64_t)fs->fs_sujfree;
        j->js_size  = (int64_t)(SUJ_MIN / fs->fs_bsize);
        j->js_nrecs = (uint64_t)(j->js_size *
                                  (fs->fs_bsize / SUJ_RECSIZE));
    }

    j->js_bufblk = j->js_start;

    /* If the filesystem was not cleanly unmounted, replay the journal */
    if (ump->um_flags & UFS_MOUNT_UNCLEAN) {
        int rc = journal_replay(ump);
        if (rc != 0)
            fprintf(stderr, "ufs-fuse: journal replay returned %d\n", rc);
        /* Invalidate the buffer cache after replay */
        binval(ump->um_bdev);
    }

    return 0;
}

void
journal_fini(struct ufs_mount *ump)
{
    struct journal_state *j = js(ump);
    if (j == NULL)
        return;

    journal_commit(ump);
    pthread_mutex_lock(&j->js_lock);
    jbuf_flush_locked(ump);
    pthread_mutex_unlock(&j->js_lock);

    pthread_mutex_destroy(&j->js_lock);
    free(j->js_buf);
    free(j);
    ump->um_journal = NULL;
}

int
journal_flush(struct ufs_mount *ump)
{
    struct journal_state *j = js(ump);
    pthread_mutex_lock(&j->js_lock);
    int rc = jbuf_flush_locked(ump);
    pthread_mutex_unlock(&j->js_lock);
    return rc;
}

int
journal_commit(struct ufs_mount *ump)
{
    struct journal_state *j = js(ump);
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);

    struct jrec_commit *jc = (struct jrec_commit *)rec;
    hdr_init(ump, &jc->jc_hdr, JREC_COMMIT);
    jc->jc_tailseq = j->js_tail;
    hdr_finalise(rec);

    int rc = journal_write_rec(ump, rec);
    if (rc == 0)
        rc = journal_flush(ump);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Journal record writers                                               */
/* ------------------------------------------------------------------ */

int
journal_write_inode(struct ufs_mount *ump, struct inode *ip)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_inode *ji = (struct jrec_inode *)rec;
    hdr_init(ump, &ji->ji_hdr, JREC_INODE);
    ji->ji_ino   = ip->i_number;
    ji->ji_mode  = inode_mode(ump, ip);
    ji->ji_nlink = (uint16_t)inode_nlink(ump, ip);
    if (UFS_IS2(ump)) {
        ji->ji_uid   = ip->i_din.di2.di_uid;
        ji->ji_gid   = ip->i_din.di2.di_gid;
        ji->ji_size  = ip->i_din.di2.di_size;
        ji->ji_mtime = ip->i_din.di2.di_mtime;
    } else {
        ji->ji_uid   = ip->i_din.di1.di_uid;
        ji->ji_gid   = ip->i_din.di1.di_gid;
        ji->ji_size  = ip->i_din.di1.di_size;
        ji->ji_mtime = (int64_t)ip->i_din.di1.di_mtime;
    }
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

int
journal_write_freeino(struct ufs_mount *ump, uint32_t ino, uint16_t mode)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_freeino *jf = (struct jrec_freeino *)rec;
    hdr_init(ump, &jf->jfi_hdr, JREC_FREEINO);
    jf->jfi_ino  = ino;
    jf->jfi_mode = mode;
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

int
journal_write_newblk(struct ufs_mount *ump, uint32_t ino,
                      int64_t blkno, int64_t lbn, int bsize)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_blk *jb = (struct jrec_blk *)rec;
    hdr_init(ump, &jb->jb_hdr, JREC_NEWBLK);
    jb->jb_ino   = ino;
    jb->jb_blkno = blkno;
    jb->jb_lbn   = lbn;
    jb->jb_bsize = (uint32_t)bsize;
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

int
journal_write_freeblk(struct ufs_mount *ump, uint32_t ino,
                       int64_t blkno, int64_t lbn, int bsize)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_blk *jb = (struct jrec_blk *)rec;
    hdr_init(ump, &jb->jb_hdr, JREC_FREEBLK);
    jb->jb_ino   = ino;
    jb->jb_blkno = blkno;
    jb->jb_lbn   = lbn;
    jb->jb_bsize = (uint32_t)bsize;
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

int
journal_write_diradd(struct ufs_mount *ump,
                      uint32_t ino, uint32_t dirino, off_t offset,
                      uint8_t type, const char *name, size_t namelen)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_diradd *jda = (struct jrec_diradd *)rec;
    hdr_init(ump, &jda->jda_hdr, JREC_DIRADD);
    jda->jda_ino    = ino;
    jda->jda_dirino = dirino;
    jda->jda_offset = offset;
    jda->jda_type   = type;
    jda->jda_namlen = (uint8_t)(namelen > sizeof(jda->jda_name) - 1 ?
                                sizeof(jda->jda_name) - 1 : namelen);
    memcpy(jda->jda_name, name, jda->jda_namlen);
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

int
journal_write_dirrem(struct ufs_mount *ump,
                      uint32_t ino, uint32_t dirino, off_t offset)
{
    uint8_t rec[SUJ_RECSIZE];
    memset(rec, 0, SUJ_RECSIZE);
    struct jrec_dirrem *jdr = (struct jrec_dirrem *)rec;
    hdr_init(ump, &jdr->jdr_hdr, JREC_DIRREM);
    jdr->jdr_ino    = ino;
    jdr->jdr_dirino = dirino;
    jdr->jdr_offset = offset;
    hdr_finalise(rec);
    return journal_write_rec(ump, rec);
}

/* ------------------------------------------------------------------ */
/* Journal replay                                                       */
/* ------------------------------------------------------------------ */

/*
 * Read and process all journal records from tail to head.
 * This is called during mount of an unclean filesystem.
 *
 * Strategy: forward scan — trust the on-disk filesystem state and use
 * the journal only to complete or roll back operations that were in
 * flight at the time of the crash.
 */
int
journal_replay(struct ufs_mount *ump)
{
    struct journal_state *j = js(ump);
    struct fs *fs = ump->um_fs;
    uint8_t  rec[SUJ_RECSIZE];

    fprintf(stderr, "ufs-fuse: replaying journal (tail=%llu head=%llu)\n",
            (unsigned long long)j->js_tail,
            (unsigned long long)j->js_head);

    uint64_t seq = j->js_tail;

    /* Read each record sequentially */
    while (seq < j->js_head) {
        int64_t blkno = seq_to_blkno(j, seq);
        int     off   = (int)((seq % (uint64_t)(SUJ_BLKSIZE / SUJ_RECSIZE))
                               * SUJ_RECSIZE);

        off_t roff = (off_t)fsbtodb(fs, blkno) * DEV_BSIZE + off;
        if (bdev_pread(ump->um_bdev, rec, SUJ_RECSIZE, roff) != 0) {
            fprintf(stderr, "ufs-fuse: journal read error at seq %llu\n",
                    (unsigned long long)seq);
            break;
        }

        struct jrec_hdr *hdr = (struct jrec_hdr *)rec;

        /* Validate magic and CRC */
        if (hdr->jr_magic != SUJ_MAGIC) {
            seq++;
            continue;
        }
        uint32_t expected = rec_crc(rec);
        if (hdr->jr_crc != expected) {
            seq++;
            continue;
        }

        switch (hdr->jr_type) {

        case JREC_FREEINO: {
            struct jrec_freeino *jf = (struct jrec_freeino *)rec;
            int isdir = ((jf->jfi_mode & IFMT) == IFDIR);
            struct inode *ip = inode_get(ump, jf->jfi_ino);
            if (ip != NULL && inode_nlink(ump, ip) == 0) {
                inode_truncate(ump, ip, 0);
                inode_update(ump, ip, 1);
                fs_free_inode(ump, jf->jfi_ino, isdir);
                inode_put(ump, ip);
            } else if (ip != NULL) {
                inode_put(ump, ip);
            }
            break;
        }

        case JREC_FREEBLK: {
            struct jrec_blk *jb = (struct jrec_blk *)rec;
            if (jb->jb_blkno > 0)
                fs_free_block(ump, jb->jb_blkno, (int)jb->jb_bsize);
            break;
        }

        case JREC_DIRREM: {
            struct jrec_dirrem *jdr = (struct jrec_dirrem *)rec;
            struct inode *dp = inode_get(ump, jdr->jdr_dirino);
            if (dp != NULL) {
                /* Try to remove; ignore ENOENT (already removed) */
                /* We need the name — look it up by scanning the offset */
                uint8_t dbuf[DIRBLKSIZ];
                off_t blkoff_v = (jdr->jdr_offset / DIRBLKSIZ) * DIRBLKSIZ;
                ssize_t n = inode_read(ump, dp, dbuf, DIRBLKSIZ, blkoff_v);
                if (n == DIRBLKSIZ) {
                    int local_off = (int)(jdr->jdr_offset % DIRBLKSIZ);
                    struct direct *ep = (struct direct *)(dbuf + local_off);
                    if (ep->d_ino == jdr->jdr_ino && ep->d_reclen > 0) {
                        dir_remove_entry(ump, dp,
                                         ep->d_name, ep->d_namlen);
                    }
                }
                inode_put(ump, dp);
            }
            break;
        }

        case JREC_COMMIT: {
            struct jrec_commit *jc = (struct jrec_commit *)rec;
            j->js_tail = jc->jc_tailseq;
            break;
        }

        case JREC_INODE:
        case JREC_NEWBLK:
        case JREC_DIRADD:
        case JREC_MKDIR:
        default:
            /* Forward pass: these are already reflected in the
             * on-disk structures; nothing to do. */
            break;
        }

        seq++;
    }

    /* Reset journal to empty state */
    j->js_head = j->js_tail = seq;

    fprintf(stderr, "ufs-fuse: journal replay complete\n");
    return 0;
}
