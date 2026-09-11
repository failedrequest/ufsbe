/*
 * journal.h — Soft-update journaling (SUJ) interface.
 *
 * SUJ (Soft Update Journaling) is FreeBSD's hybrid consistency mechanism:
 * it layers a small write-ahead circular log on top of soft updates.  Instead
 * of journaling every data change, it journals only the soft-updates dependency
 * records themselves.  On a clean unmount no journal is needed; after a crash
 * only the journal needs to be replayed, which takes seconds regardless of
 * filesystem size.
 *
 * On-disk layout (matches FreeBSD sys/ufs/ffs/softdep.h SUJ records):
 *
 *   The journal lives in a special file whose inode number is stored in
 *   fs_sujfree (a field in the superblock added for SUJ).  The journal
 *   file is pre-allocated contiguously and treated as a circular log.
 *
 *   Each journal record is SUJ_RECSIZE (128) bytes, prefixed by a common
 *   header (jrec_hdr) that contains a sequence number, record type, and
 *   CRC32.
 *
 * Record types:
 *   JREC_INODE   — inode allocation or modification
 *   JREC_FREEINO — inode free
 *   JREC_NEWBLK  — block allocation
 *   JREC_FREEBLK — block free
 *   JREC_DIRADD  — directory entry added
 *   JREC_DIRREM  — directory entry removed
 *   JREC_MKDIR   — mkdir (new directory created)
 *
 * Recovery:
 *   On mount of an unclean filesystem with SUJ enabled, journal_replay()
 *   walks the circular log from the oldest unconfirmed sequence number to
 *   the most recent, applying or rolling back each record as needed.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef JOURNAL_H
#define JOURNAL_H

#include <stdint.h>
#include <stddef.h>
#include "ufs_mount.h"
#include "ufs_inode.h"

/* ------------------------------------------------------------------ */
/* Constants                                                            */
/* ------------------------------------------------------------------ */

#define SUJ_RECSIZE         128     /* bytes per journal record */
#define SUJ_BLKSIZE         32768   /* journal I/O unit (32 KiB) */
#define SUJ_MIN_RECS        1024    /* minimum record count */
#define SUJ_MAGIC           0x4a4f5552554e4c53ULL  /* "SJRNLUOF" */

/* ------------------------------------------------------------------ */
/* Record types                                                         */
/* ------------------------------------------------------------------ */

#define JREC_INODE      1   /* inode allocated / modified */
#define JREC_FREEINO    2   /* inode freed */
#define JREC_NEWBLK     3   /* block allocated */
#define JREC_FREEBLK    4   /* block freed */
#define JREC_DIRADD     5   /* directory entry added */
#define JREC_DIRREM     6   /* directory entry removed */
#define JREC_MKDIR      7   /* mkdir */
#define JREC_COMMIT     8   /* commit point (sequence flush) */

/* ------------------------------------------------------------------ */
/* On-disk record structures (all exactly SUJ_RECSIZE bytes)            */
/* ------------------------------------------------------------------ */

/* Common 24-byte header at the start of every record */
struct jrec_hdr {
    uint64_t    jr_magic;       /* SUJ_MAGIC */
    uint64_t    jr_seq;         /* monotonic sequence number */
    uint32_t    jr_type;        /* JREC_* */
    uint32_t    jr_crc;         /* CRC32 of the full record (crc field=0) */
};

struct jrec_inode {
    struct jrec_hdr  ji_hdr;
    uint32_t         ji_ino;
    uint16_t         ji_mode;
    uint16_t         ji_nlink;
    uint32_t         ji_uid;
    uint32_t         ji_gid;
    uint64_t         ji_size;
    int64_t          ji_mtime;
    uint8_t          ji_pad[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 40];
};

struct jrec_freeino {
    struct jrec_hdr  jfi_hdr;
    uint32_t         jfi_ino;
    uint16_t         jfi_mode;
    uint8_t          jfi_pad[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 6];
};

struct jrec_blk {
    struct jrec_hdr  jb_hdr;
    uint32_t         jb_ino;        /* owning inode (for NEWBLK/FREEBLK) */
    int64_t          jb_blkno;
    int64_t          jb_lbn;
    uint32_t         jb_bsize;
    uint8_t          jb_pad[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 28];
};

struct jrec_diradd {
    struct jrec_hdr  jda_hdr;
    uint32_t         jda_ino;       /* new entry inode */
    uint32_t         jda_dirino;    /* directory inode */
    int64_t          jda_offset;    /* byte offset in directory */
    uint8_t          jda_type;
    uint8_t          jda_namlen;
    char             jda_name[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 18];
};

struct jrec_dirrem {
    struct jrec_hdr  jdr_hdr;
    uint32_t         jdr_ino;
    uint32_t         jdr_dirino;
    int64_t          jdr_offset;
    uint8_t          jdr_pad[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 16];
};

struct jrec_commit {
    struct jrec_hdr  jc_hdr;
    uint64_t         jc_tailseq;    /* oldest active sequence */
    uint8_t          jc_pad[SUJ_RECSIZE - sizeof(struct jrec_hdr) - 8];
};

/* ------------------------------------------------------------------ */
/* In-core journal state                                                */
/* ------------------------------------------------------------------ */

struct journal_state {
    /* Journal file: contiguous blocks starting at jnl_start */
    int64_t     js_start;       /* first FS block of journal */
    int64_t     js_size;        /* journal size in blocks */
    uint64_t    js_nrecs;       /* total record slots */

    /* Circular log pointers */
    uint64_t    js_head;        /* next record to write (seq number) */
    uint64_t    js_tail;        /* oldest unconfirmed record */

    /* Write buffer (one SUJ_BLKSIZE chunk) */
    uint8_t    *js_buf;
    int64_t     js_bufblk;      /* first FS block of js_buf */
    int         js_bufoff;      /* next byte offset within js_buf */
    int         js_bufdirty;

    pthread_mutex_t js_lock;
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/* Initialise the journal: reads the journal inode, locates extents.
 * If the filesystem is unclean, calls journal_replay() automatically. */
int  journal_init(struct ufs_mount *ump);

/* Tear down and flush the journal. */
void journal_fini(struct ufs_mount *ump);

/* Flush all buffered journal records to disk (does NOT commit). */
int  journal_flush(struct ufs_mount *ump);

/* Write a commit record, making all preceding records permanent. */
int  journal_commit(struct ufs_mount *ump);

/* Replay the journal after an unclean mount. */
int  journal_replay(struct ufs_mount *ump);

/*
 * Journal record writers — called by the FUSE ops layer and softdep.
 * Each returns 0 on success or -errno.
 */
int journal_write_inode(struct ufs_mount *ump, struct inode *ip);
int journal_write_freeino(struct ufs_mount *ump, uint32_t ino, uint16_t mode);
int journal_write_newblk(struct ufs_mount *ump, uint32_t ino,
                          int64_t blkno, int64_t lbn, int bsize);
int journal_write_freeblk(struct ufs_mount *ump, uint32_t ino,
                           int64_t blkno, int64_t lbn, int bsize);
int journal_write_diradd(struct ufs_mount *ump,
                          uint32_t ino, uint32_t dirino, off_t offset,
                          uint8_t type, const char *name, size_t namelen);
int journal_write_dirrem(struct ufs_mount *ump,
                          uint32_t ino, uint32_t dirino, off_t offset);

#endif /* JOURNAL_H */
