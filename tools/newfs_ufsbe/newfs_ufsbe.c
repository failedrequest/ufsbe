/*
 * newfs_ufsbe.c — Create a UFS2 filesystem marked for B-epsilon tree use.
 *
 * This tool is a thin front-end that:
 *   1. Runs the standard newfs(8) logic via a fork+exec to lay down a
 *      correct UFS2 filesystem with soft updates.
 *   2. Opens the resulting superblock and OR-in FS_BETREE (0x00000800).
 *   3. Migrates the small number of inodes that newfs created (root dir,
 *      .snap dir, etc.) from di_db[]/di_ib[] pointers to B-epsilon blkmap
 *      trees stored in di_extb[0], so the filesystem is immediately
 *      consistent with the FS_BETREE invariant.
 *   4. Rewrites the superblock with sbput().
 *
 * The resulting filesystem is identical to one produced by:
 *   newfs -O 2 -U <device>
 * except that fs_flags has FS_BETREE set and all inodes use B-epsilon
 * block maps.
 *
 * Usage:
 *   newfs_ufsbe [-U] [-j] [-b bsize] [-f fsize] [-s size] <device>
 *
 *   -U        Enable soft updates (default: on)
 *   -N        No-op: print what would be done, do not write
 *   -j        Enable soft-update journaling (SUJ)
 *   -b bsize  Block size (default: 32768)
 *   -f fsize  Fragment size (default: 4096)
 *   -s size   Filesystem size in sectors (0 = whole device)
 *   -L label  Volume label
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <ufs/ufs/dinode.h>
#include <ufs/ffs/fs.h>
#include <libufs.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* FS_BETREE: B-epsilon tree block map + directory index.
 * Next free bit after FS_TRIM (0x400) within FS_SUPPORTED (0x00FFFFFF). */
#ifndef FS_BETREE
#define FS_BETREE   0x00000800
#endif

/* On-disk B-epsilon node header (must match betree.h / fsck_ufsbe.c) */
#define BE_NODE_MAGIC   0x42455472UL    /* "BETr" */
#define BE_NODE_LEAF    1
#define BE_BUF_MAX      256
#define BE_KEY_MAX      512

struct be_node_hdr {
    uint32_t bn_magic;
    uint8_t  bn_type;
    uint8_t  bn_pad[3];
    uint32_t bn_nkeys;
    uint32_t bn_nbuf;
};

struct be_msg {
    uint8_t  bm_op;
    uint8_t  bm_pad[7];
    int64_t  bm_key;
    int64_t  bm_val;
};

/* ------------------------------------------------------------------ */
/* B-epsilon leaf node geometry (mirrors betree_geometry in betree.c)  */
/* ------------------------------------------------------------------ */

static void
be_geometry(int bsize, int *buf_max_out, int *leaf_max_out)
{
    int usable    = bsize - (int)sizeof(struct be_node_hdr);
    int buf_bytes = usable / 4;
    int main_bytes = usable - buf_bytes;

    int nbuf  = buf_bytes / (int)sizeof(struct be_msg);
    if (nbuf  > BE_BUF_MAX) nbuf  = BE_BUF_MAX;
    int nleaf = main_bytes / 16;   /* key(8) + val(8) */
    if (nleaf > BE_KEY_MAX) nleaf = BE_KEY_MAX;
    if (nbuf  < 4) nbuf  = 4;
    if (nleaf < 4) nleaf = 4;

    *buf_max_out  = nbuf;
    *leaf_max_out = nleaf;
}

/* ------------------------------------------------------------------ */
/* Block I/O helpers                                                    */
/* ------------------------------------------------------------------ */

static int gfd;             /* global device fd, set in migrate_inodes */
static struct fs *gfs;      /* global superblock pointer */

static void *
read_fsblock(ufs2_daddr_t blkno)
{
    void   *buf = malloc((size_t)gfs->fs_bsize);
    off_t   off = (off_t)blkno * (off_t)gfs->fs_fsize;
    ssize_t n;

    if (!buf) err(1, "malloc");
    n = pread(gfd, buf, (size_t)gfs->fs_bsize, off);
    if (n != (ssize_t)gfs->fs_bsize) {
        free(buf);
        return NULL;
    }
    return buf;
}

static int
write_fsblock(ufs2_daddr_t blkno, const void *buf)
{
    off_t   off = (off_t)blkno * (off_t)gfs->fs_fsize;
    ssize_t n   = pwrite(gfd, buf, (size_t)gfs->fs_bsize, off);
    return (n == (ssize_t)gfs->fs_bsize) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* CG allocator: find and mark one free full block in cylinder group   */
/* ------------------------------------------------------------------ */

/*
 * alloc_block: allocate one free filesystem block anywhere on the device.
 * Returns the allocated block number, or 0 on failure.
 *
 * Strategy: iterate CGs, read each CG, scan blksfree for a free block,
 * mark it allocated, write the CG back.
 */
static ufs2_daddr_t
alloc_block(void)
{
    for (uint32_t cgno = 0; cgno < (uint32_t)gfs->fs_ncg; cgno++) {
        struct cg *cgp = malloc((size_t)gfs->fs_cgsize);
        if (!cgp) err(1, "malloc");

        if (cgget(gfd, gfs, (int)cgno, cgp) != 0) {
            free(cgp);
            continue;
        }

        if (cgp->cg_cs.cs_nbfree == 0) {
            free(cgp);
            continue;
        }

        uint8_t      *blksfree = cg_blksfree(cgp);
        ufs2_daddr_t  base     = (ufs2_daddr_t)cgbase(gfs, cgno);
        int           ndblk    = (int)cgp->cg_ndblk;

        /* Scan for a free full block (fs_frag fragments at a time) */
        int frag;
        for (frag = 0; frag < ndblk; frag += gfs->fs_frag) {
            /* Check all fragments in this block are free */
            int all_free = 1;
            for (int fi = 0; fi < gfs->fs_frag; fi++) {
                int bit = frag + fi;
                if (bit >= ndblk ||
                    !((blksfree[bit/8] >> (bit%8)) & 1)) {
                    all_free = 0;
                    break;
                }
            }
            if (all_free) break;
        }

        if (frag >= ndblk) {
            free(cgp);
            continue;
        }

        /* Mark all fragments of this block as allocated */
        for (int fi = 0; fi < gfs->fs_frag; fi++) {
            int bit = frag + fi;
            blksfree[bit/8] &= (uint8_t)~(1u << (bit % 8));
        }
        cgp->cg_cs.cs_nbfree--;
        gfs->fs_cstotal.cs_nbfree--;
        gfs->fs_cs(gfs, cgno).cs_nbfree--;

        if (cgput(gfd, gfs, cgp) != 0)
            warn("cgput cg %u", cgno);

        free(cgp);
        ufs2_daddr_t blkno = base + (ufs2_daddr_t)frag;
        return blkno;
    }
    return 0;   /* no free block found */
}

/* ------------------------------------------------------------------ */
/* Write a single BE leaf node block with up to nkeys (lbn,pbn) pairs  */
/* ------------------------------------------------------------------ */

/*
 * write_be_leaf: write a single B-epsilon leaf node containing the
 * provided key/value pairs to disk block `blkno`.
 *
 * The layout (mirrors betree.c betree_node_write):
 *   [be_node_hdr][buf_max × be_msg (all zeroed)][leaf_max × key][leaf_max × val]
 */
static int
write_be_leaf(ufs2_daddr_t blkno, const int64_t *keys, const int64_t *vals,
              int nkeys, int buf_max, int leaf_max)
{
    uint8_t *buf = calloc(1, (size_t)gfs->fs_bsize);
    if (!buf) err(1, "calloc");

    struct be_node_hdr hdr = {
        .bn_magic = BE_NODE_MAGIC,
        .bn_type  = BE_NODE_LEAF,
        .bn_nkeys = (uint32_t)nkeys,
        .bn_nbuf  = 0,
    };
    memcpy(buf, &hdr, sizeof(hdr));

    uint8_t *p = buf + sizeof(hdr);

    /* Buffer area: buf_max × be_msg, all zeros (no buffered messages) */
    p += (size_t)buf_max * sizeof(struct be_msg);

    /* Keys array: leaf_max entries, only first nkeys are valid */
    memcpy(p, keys, (size_t)nkeys * sizeof(int64_t));
    p += (size_t)leaf_max * sizeof(int64_t);

    /* Values array: leaf_max entries */
    memcpy(p, vals, (size_t)nkeys * sizeof(int64_t));

    int rc = write_fsblock(blkno, buf);
    free(buf);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Inode migration: di_db[]/di_ib[] → B-epsilon blkmap tree           */
/* ------------------------------------------------------------------ */

/*
 * collect_direct_blocks: read the direct block pointers (di_db[]) and
 * the first indirect block (di_ib[0]) from a UFS2 dinode and return an
 * array of (lbn, pbn) pairs.  Indirect levels beyond 1 are not supported
 * here because newfs never creates files that large.
 *
 * Returns number of pairs written into keys[]/vals[].
 */
static int
collect_blocks(const struct ufs2_dinode *dip,
               int64_t *keys, int64_t *vals, int max_pairs)
{
    int n = 0;

    /* Direct blocks */
    for (int i = 0; i < UFS_NDADDR && n < max_pairs; i++) {
        if (dip->di_db[i] != 0) {
            keys[n] = (int64_t)i;
            vals[n] = (int64_t)dip->di_db[i];
            n++;
        }
    }

    /* Single-indirect (di_ib[0]) — rare for newfs output but handle it */
    if (n < max_pairs && dip->di_ib[0] != 0) {
        uint8_t *indbuf = read_fsblock((ufs2_daddr_t)dip->di_ib[0]);
        if (indbuf) {
            int nindir = gfs->fs_bsize / (int)sizeof(ufs2_daddr_t);
            ufs2_daddr_t *ptrs = (ufs2_daddr_t *)(void *)indbuf;
            for (int i = 0; i < nindir && n < max_pairs; i++) {
                if (ptrs[i] != 0) {
                    keys[n] = (int64_t)(UFS_NDADDR + i);
                    vals[n] = (int64_t)ptrs[i];
                    n++;
                }
            }
            free(indbuf);
        }
    }

    return n;
}

/*
 * migrate_inodes: walk every allocated inode in the freshly-newfs'd
 * filesystem.  For each that has non-zero di_db[]/di_ib[]:
 *   1. Collect (lbn, pbn) pairs.
 *   2. Allocate a free block for the BE leaf node.
 *   3. Write the leaf node.
 *   4. Set di_extb[0] = leaf blkno, clear di_db[]/di_ib[].
 *   5. Rewrite the inode block.
 */
static void
migrate_inodes(int verbose)
{
    int buf_max, leaf_max;
    be_geometry(gfs->fs_bsize, &buf_max, &leaf_max);

    ino_t maxino = (ino_t)gfs->fs_ncg * (ino_t)gfs->fs_ipg;
    int   nmigrated = 0;

    /* Temporary key/value arrays: at most leaf_max pairs */
    int64_t *keys = malloc((size_t)leaf_max * sizeof(int64_t));
    int64_t *vals = malloc((size_t)leaf_max * sizeof(int64_t));
    if (!keys || !vals) err(1, "malloc");

    for (ino_t ino = UFS_ROOTINO; ino < maxino; ino++) {
        ufs2_daddr_t iblkno = (ufs2_daddr_t)ino_to_fsba(gfs, ino);
        int          ioff   = (int)ino_to_fsbo(gfs, ino);

        uint8_t *iblk = read_fsblock(iblkno);
        if (!iblk) {
            warn("cannot read inode block %"PRId64, (int64_t)iblkno);
            continue;
        }

        struct ufs2_dinode *dip = (struct ufs2_dinode *)iblk + ioff;

        /* Skip unallocated */
        if (dip->di_mode == 0) {
            free(iblk);
            continue;
        }

        /* Check whether di_db[]/di_ib[] are non-zero */
        int db_nonzero = 0, ib_nonzero = 0;
        for (int i = 0; i < UFS_NDADDR; i++)
            if (dip->di_db[i]) { db_nonzero = 1; break; }
        for (int i = 0; i < UFS_NIADDR; i++)
            if (dip->di_ib[i]) { ib_nonzero = 1; break; }

        if (!db_nonzero && !ib_nonzero) {
            /* Already migrated or empty inode — nothing to do */
            free(iblk);
            continue;
        }

        /* Collect (lbn, pbn) pairs */
        int npairs = collect_blocks(dip, keys, vals, leaf_max);

        if (npairs == 0) {
            /* No actual data blocks; just zero the pointers */
            memset(dip->di_db, 0, sizeof(dip->di_db));
            memset(dip->di_ib, 0, sizeof(dip->di_ib));
            if (write_fsblock(iblkno, iblk) != 0)
                warn("write inode block %"PRId64, (int64_t)iblkno);
            free(iblk);
            nmigrated++;
            continue;
        }

        /* Allocate a free block for the BE leaf node */
        ufs2_daddr_t leaf_blkno = alloc_block();
        if (leaf_blkno == 0) {
            warnx("no free block for BE leaf node (ino %lu)",
                  (unsigned long)ino);
            free(iblk);
            continue;
        }

        /* Write the BE leaf node */
        if (write_be_leaf(leaf_blkno, keys, vals, npairs,
                          buf_max, leaf_max) != 0) {
            warn("write BE leaf block %"PRId64, (int64_t)leaf_blkno);
            free(iblk);
            continue;
        }

        /* Update the inode: set di_extb[0], clear di_db[]/di_ib[] */
        /*
         * di_extb[] is declared as int64_t[UFS_NXADDR] in dinode.h.
         * di_extsize == 0 means no extended attributes; we repurpose
         * di_extb[0] for the blkmap root and di_extb[1] for dir-index.
         * This matches the ufsbe convention (and fsck_ufsbe expectations).
         */
        dip->di_extb[0] = (int64_t)leaf_blkno;
        memset(dip->di_db, 0, sizeof(dip->di_db));
        memset(dip->di_ib, 0, sizeof(dip->di_ib));

        if (write_fsblock(iblkno, iblk) != 0)
            warn("write inode block %"PRId64, (int64_t)iblkno);

        if (verbose)
            printf("newfs_ufsbe: migrated ino %lu: "
                   "%d block(s) → BE leaf at blkno %"PRId64"\n",
                   (unsigned long)ino, npairs, (int64_t)leaf_blkno);

        nmigrated++;
        free(iblk);
    }

    free(keys);
    free(vals);

    if (verbose)
        printf("newfs_ufsbe: migrated %d inode(s) to B-epsilon blkmaps\n",
               nmigrated);
}

/* ------------------------------------------------------------------ */
/* Usage                                                                */
/* ------------------------------------------------------------------ */

static void
usage(void)
{
    fprintf(stderr,
        "usage: newfs_ufsbe [-NUj] [-b bsize] [-f fsize] [-s sectors]\n"
        "                   [-L label] <device>\n"
        "\n"
        "  -N        dry-run: print newfs command, do not execute\n"
        "  -U        enable soft updates (default: on)\n"
        "  -j        enable soft-update journaling (SUJ)\n"
        "  -b bsize  block size (default 32768)\n"
        "  -f fsize  fragment size (default 4096)\n"
        "  -s size   filesystem size in sectors (default: whole device)\n"
        "  -L label  volume label\n"
        "\n"
        "newfs_ufsbe creates a standard UFS2+softdep filesystem and then\n"
        "sets the FS_BETREE flag (0x%08x) in the superblock, signalling\n"
        "that ufsbe should use B-epsilon trees for all inode block maps\n"
        "and directory indexes.  Use fsck_ufsbe, not fsck_ffs, to check\n"
        "these filesystems.\n",
        FS_BETREE);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
    int    Nflag   = 0;   /* dry-run */
    int    Uflag   = 1;   /* soft updates: on by default */
    int    jflag   = 0;   /* SUJ */
    int    bsize   = 32768;
    int    fsize   = 4096;
    long   sectors = 0;
    char  *label   = NULL;
    int    ch;

    while ((ch = getopt(argc, argv, "NUjb:f:s:L:")) != -1) {
        switch (ch) {
        case 'N': Nflag   = 1;               break;
        case 'U': Uflag   = 1;               break;
        case 'j': jflag   = 1;               break;
        case 'b': bsize   = (int)atol(optarg); break;
        case 'f': fsize   = (int)atol(optarg); break;
        case 's': sectors = atol(optarg);    break;
        case 'L': label   = optarg;          break;
        default:  usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1)
        usage();

    const char *device = argv[0];

    /* ----------------------------------------------------------------
     * Step 1: build the newfs(8) argument list and fork+exec it.
     * ---------------------------------------------------------------- */
    char bsize_s[32], fsize_s[32], sectors_s[32];
    snprintf(bsize_s,   sizeof(bsize_s),   "%d", bsize);
    snprintf(fsize_s,   sizeof(fsize_s),   "%d", fsize);
    snprintf(sectors_s, sizeof(sectors_s), "%ld", sectors);

    const char *nf_argv[32];
    int         nf_argc = 0;

    nf_argv[nf_argc++] = "newfs";
    nf_argv[nf_argc++] = "-O"; nf_argv[nf_argc++] = "2";
    nf_argv[nf_argc++] = "-b"; nf_argv[nf_argc++] = bsize_s;
    nf_argv[nf_argc++] = "-f"; nf_argv[nf_argc++] = fsize_s;
    if (Uflag)
        nf_argv[nf_argc++] = "-U";
    if (jflag)
        nf_argv[nf_argc++] = "-j";
    if (label) {
        nf_argv[nf_argc++] = "-L";
        nf_argv[nf_argc++] = label;
    }
    if (sectors > 0) {
        nf_argv[nf_argc++] = "-s";
        nf_argv[nf_argc++] = sectors_s;
    }
    nf_argv[nf_argc++] = device;
    nf_argv[nf_argc]   = NULL;

    printf("newfs_ufsbe: running:");
    for (int i = 0; i < nf_argc; i++)
        printf(" %s", nf_argv[i]);
    printf("\n");

    if (Nflag) {
        printf("newfs_ufsbe: dry-run; not writing filesystem.\n");
        printf("newfs_ufsbe: would then set FS_BETREE (0x%08x) on %s\n",
               FS_BETREE, device);
        return 0;
    }

    /* Fork and exec newfs */
    pid_t pid = fork();
    if (pid == -1) err(1, "fork");

    if (pid == 0) {
        execvp("newfs", (char * const *)(uintptr_t)nf_argv);
        err(1, "exec newfs");
    }

    int status;
    if (waitpid(pid, &status, 0) == -1) err(1, "waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        errx(1, "newfs failed with status %d", WEXITSTATUS(status));

    /* ----------------------------------------------------------------
     * Step 2: open the freshly-created filesystem.
     * ---------------------------------------------------------------- */
    gfd = open(device, O_RDWR);
    if (gfd == -1) err(1, "open(%s)", device);

    int rc = sbget(gfd, &gfs, UFS_STDSB, UFS_NOHASHFAIL | UFS_NOMSG);
    if (rc != 0) err(1, "sbget(%s)", device);

    if (gfs->fs_magic != FS_UFS2_MAGIC)
        errx(1, "%s: not a UFS2 filesystem (magic 0x%x)",
             device, gfs->fs_magic);

    printf("newfs_ufsbe: %s: fs_flags before: 0x%08x\n",
           device, gfs->fs_flags);

    /* ----------------------------------------------------------------
     * Step 3: migrate existing inodes from di_db[]/di_ib[] to B-ε trees.
     * This must happen BEFORE setting FS_BETREE so that the allocator
     * (alloc_block) works against the live CG bitmaps.
     * ---------------------------------------------------------------- */
    migrate_inodes(1 /* verbose */);

    /* ----------------------------------------------------------------
     * Step 4: set FS_BETREE in the superblock and write it back.
     * ---------------------------------------------------------------- */
    gfs->fs_flags |= FS_BETREE;

    rc = sbput(gfd, gfs, 0);
    if (rc != 0) err(1, "sbput(%s)", device);

    printf("newfs_ufsbe: %s: fs_flags after:  0x%08x (FS_BETREE set)\n",
           device, gfs->fs_flags);

    free(gfs);
    close(gfd);
    return 0;
}
