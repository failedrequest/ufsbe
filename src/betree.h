/*
 * betree.h — Persistent B-epsilon tree for ufs-fuse.
 *
 * A B-epsilon tree (Brodal & Fagerberg 1996, Bender et al. 2007) is a
 * cache-oblivious write-optimised tree that adds a write buffer at every
 * internal node.  Inserts accumulate in the root buffer and flush lazily
 * downward in batches, amortising the I/O cost of writes while keeping
 * point-query performance at O(log_B N).
 *
 * This implementation is a persistent, block-device-backed B-ε tree with:
 *   - Fixed node size equal to one filesystem block (fs_bsize)
 *   - int64_t keys and int64_t values
 *   - Two message types: BE_MSG_INSERT and BE_MSG_DELETE
 *   - Internal nodes: pivots + child block pointers + insert buffer
 *   - Leaf nodes: sorted key/value pairs, no buffer
 *   - Nodes are allocated via the UFS block allocator (fs_alloc_block)
 *   - Nodes are cached by the existing buffer cache (buf_bread / bdwrite)
 *
 * USAGE
 * -----
 *   struct betree bt;
 *   betree_init(&bt, ump, root_blkno);   // root_blkno==0 → empty tree
 *   betree_insert(&bt, key, value);
 *   betree_delete(&bt, key);
 *   betree_lookup(&bt, key, &value);     // 0 = found, -ENOENT = missing
 *   betree_range(&bt, lo, hi, cb, arg); // iterate [lo, hi]
 *   betree_destroy(&bt);                 // free all nodes
 *
 * The caller is responsible for persisting the root block number (e.g. in
 * the inode's spare fields) after any modification.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef BETREE_H
#define BETREE_H

#include <stdint.h>
#include <stddef.h>
#include "ufs_mount.h"

/* ------------------------------------------------------------------ */
/* On-disk node header (16 bytes)                                       */
/* ------------------------------------------------------------------ */

#define BE_NODE_MAGIC   0x42455472UL    /* "BETr" */

#define BE_NODE_INTERNAL  0
#define BE_NODE_LEAF      1

struct be_node_hdr {
    uint32_t    bn_magic;       /* BE_NODE_MAGIC */
    uint8_t     bn_type;        /* BE_NODE_INTERNAL or BE_NODE_LEAF */
    uint8_t     bn_pad[3];
    uint32_t    bn_nkeys;       /* number of pivot/leaf keys in use */
    uint32_t    bn_nbuf;        /* number of buffered messages (internal only) */
};

/* ------------------------------------------------------------------ */
/* Message types stored in the insert buffer of internal nodes          */
/* ------------------------------------------------------------------ */

#define BE_MSG_INSERT   1
#define BE_MSG_DELETE   2

struct be_msg {
    uint8_t     bm_op;          /* BE_MSG_INSERT or BE_MSG_DELETE */
    uint8_t     bm_pad[7];
    int64_t     bm_key;
    int64_t     bm_val;         /* only meaningful for INSERT */
};

/* ------------------------------------------------------------------ */
/* Node layout constants (derived from a typical 4096-byte block)       */
/*                                                                      */
/* For a block of size B bytes:                                         */
/*   header:    sizeof(be_node_hdr)  = 16 bytes                        */
/*   epsilon:   1/4 of usable space is for insert buffer               */
/*   rest:      keys + children (internal) or key/value pairs (leaf)   */
/*                                                                      */
/* These are computed at runtime from fs_bsize; the constants below     */
/* are upper bounds used to size the in-core representation.            */
/* ------------------------------------------------------------------ */

#define BE_MAX_BLOCK_SIZE   65536       /* largest supported fs_bsize */

/* At 4096-byte block:
 *   usable = 4096 - 16 = 4080 bytes
 *   buffer = 4080/4 = 1020 bytes → 63 messages (each 16 bytes)
 *   pivots = 3060 bytes:
 *     internal: key(8) + child(8) = 16 bytes/entry → 191 entries max
 *     leaf:     key(8) + val(8)   = 16 bytes/entry → 191 pairs max
 */
#define BE_BUF_MAX          256     /* absolute upper bound on buffer msgs */
#define BE_KEY_MAX          512     /* absolute upper bound on keys/node */

/* ------------------------------------------------------------------ */
/* In-core node (loaded on demand, cached via buffer cache)             */
/* ------------------------------------------------------------------ */

struct be_node {
    int64_t     bn_blkno;           /* FS block this node lives on */
    uint8_t     bn_type;            /* internal or leaf */
    uint32_t    bn_nkeys;           /* keys in use */
    uint32_t    bn_nbuf;            /* messages in buffer */

    int64_t     bn_keys[BE_KEY_MAX];        /* pivot or leaf keys */
    int64_t     bn_vals[BE_KEY_MAX];        /* values (leaf) or child blknos (internal) */
    int64_t     bn_children[BE_KEY_MAX+1];  /* child[0..nkeys] for internal nodes */

    struct be_msg bn_buf[BE_BUF_MAX];       /* insert buffer (internal only) */
};

/* ------------------------------------------------------------------ */
/* Tree handle                                                          */
/* ------------------------------------------------------------------ */

struct betree {
    struct ufs_mount   *bt_ump;         /* owning mount */
    int64_t             bt_root;        /* root node block number, 0=empty */

    /* per-tree layout constants (computed from fs_bsize at init) */
    int                 bt_bsize;       /* block size */
    int                 bt_leaf_max;    /* max key/value pairs per leaf */
    int                 bt_int_max;     /* max pivots per internal node */
    int                 bt_buf_max;     /* max messages in insert buffer */
};

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

/*
 * Initialise a B-ε tree handle.
 * root_blkno: the FS block number of the root node, or 0 for an empty tree.
 * Reads the block size from ump->um_fs->fs_bsize and derives node geometry.
 */
void betree_init(struct betree *bt, struct ufs_mount *ump, int64_t root_blkno);

/*
 * Insert or overwrite key → value.
 * Returns 0 on success, -ENOSPC if out of blocks, -EIO on I/O error.
 * On return bt->bt_root may have changed (root split).
 */
int betree_insert(struct betree *bt, int64_t key, int64_t value);

/*
 * Delete key (no-op if key not present).
 * Returns 0 on success, -EIO on I/O error.
 */
int betree_delete(struct betree *bt, int64_t key);

/*
 * Look up key.  On success stores the value in *val_out and returns 0.
 * Returns -ENOENT if not found, -EIO on I/O error.
 */
int betree_lookup(struct betree *bt, int64_t key, int64_t *val_out);

/*
 * Range scan: call cb(arg, key, value) for every key in [lo, hi] in
 * ascending order.  If cb returns non-zero the scan stops and that value
 * is returned.  Returns 0 when the full range has been scanned.
 */
typedef int (*betree_cb_t)(void *arg, int64_t key, int64_t value);
int betree_range(struct betree *bt, int64_t lo, int64_t hi,
                 betree_cb_t cb, void *arg);

/*
 * Free all nodes of the tree (releases all FS blocks).
 * bt->bt_root is set to 0 on return.
 */
void betree_destroy(struct betree *bt);

#endif /* BETREE_H */
