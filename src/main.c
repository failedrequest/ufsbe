/*
 * main.c — ufs-fuse entry point.
 *
 * Usage:
 *   ufs-fuse [options] <device|image> <mountpoint>
 *
 * Options:
 *   -o ro          Mount read-only
 *   -o softdep     Enable soft updates (default if superblock has FS_DOSOFTDEP)
 *   -o nosoftdep   Disable soft updates
 *   -o journal     Enable soft-update journaling (default if SB has FS_SUJ)
 *   -o nojournal   Disable journaling
 *   -o cache=N     Set buffer cache size to N buffers (default 4096)
 *   -f             Foreground mode (do not daemonise)
 *   -d             Debug mode (implies -f)
 *   -s             Single-threaded mode
 *
 * All standard FUSE options (-o allow_other, etc.) are also accepted.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>

#include "ufs_mount.h"
#include "ufs_fs.h"
#include "block.h"
#include "super.h"
#include "inode.h"
#include "softdep.h"
#include "journal.h"
#include "fuse_ops.h"

/* ------------------------------------------------------------------ */
/* ufs-fuse specific options                                            */
/* ------------------------------------------------------------------ */

struct ufs_fuse_opts {
    int    rdonly;
    int    softdep;      /* +1 = force on, -1 = force off, 0 = auto */
    int    journal;      /* +1 = force on, -1 = force off, 0 = auto */
    int    cache_bufs;   /* buffer cache size in buffers */
    char  *device;
};

/* ------------------------------------------------------------------ */
/* Mount helper                                                         */
/* ------------------------------------------------------------------ */

static struct ufs_mount *
do_mount(const struct ufs_fuse_opts *opts)
{
    struct ufs_mount *ump = calloc(1, sizeof(*ump));
    if (ump == NULL) {
        fprintf(stderr, "ufs-fuse: out of memory\n");
        return NULL;
    }

    ump->um_devpath = opts->device;

    if (opts->rdonly)
        ump->um_flags |= UFS_MOUNT_RDONLY;

    /* Open block device / image */
    uint32_t cache_bufs = (opts->cache_bufs > 0) ?
        (uint32_t)opts->cache_bufs : BCACHE_DEFAULT_BUFS;

    ump->um_bdev = bdev_open(opts->device,
                              (ump->um_flags & UFS_MOUNT_RDONLY) != 0,
                              DEV_BSIZE,   /* initial; updated after SB read */
                              cache_bufs);
    if (ump->um_bdev == NULL) {
        fprintf(stderr, "ufs-fuse: cannot open %s: %s\n",
                opts->device, strerror(errno));
        free(ump);
        return NULL;
    }

    /* Read and validate superblock */
    int rc = super_read(ump);
    if (rc != 0) {
        fprintf(stderr, "ufs-fuse: cannot read superblock from %s: %s\n",
                opts->device, strerror(-rc));
        bdev_close(ump->um_bdev);
        free(ump);
        return NULL;
    }

    /* Update bdev block size now that we know fs_fsize */
    ump->um_bdev->bd_blksize = (uint32_t)ump->um_fs->fs_fsize;

    /* Override softdep / journal from options */
    if (opts->softdep > 0)
        ump->um_flags |= UFS_MOUNT_SOFTDEP;
    else if (opts->softdep < 0)
        ump->um_flags &= ~UFS_MOUNT_SOFTDEP;

    if (opts->journal > 0)
        ump->um_flags |= UFS_MOUNT_JOURNAL;
    else if (opts->journal < 0)
        ump->um_flags &= ~UFS_MOUNT_JOURNAL;

    fprintf(stderr, "ufs-fuse: mounted %s (UFS%d%s%s)\n",
            opts->device,
            (ump->um_flags & UFS_MOUNT_UFS2) ? 2 : 1,
            (ump->um_flags & UFS_MOUNT_SOFTDEP) ? " softdep" : "",
            (ump->um_flags & UFS_MOUNT_JOURNAL) ? "+journal" : "");

    if (ump->um_flags & UFS_MOUNT_UNCLEAN)
        fprintf(stderr, "ufs-fuse: WARNING: filesystem was not cleanly "
                "unmounted\n");

    /* Initialise inode cache */
    inode_cache_init(ump);

    /* Initialise soft updates */
    if (UFS_SOFTDEP(ump)) {
        rc = softdep_init(ump);
        if (rc != 0) {
            fprintf(stderr, "ufs-fuse: softdep_init failed: %s\n",
                    strerror(-rc));
            /* Non-fatal: continue without soft updates */
            ump->um_flags &= ~UFS_MOUNT_SOFTDEP;
        }
    }

    /* Initialise journal (also triggers replay if unclean) */
    if (UFS_JOURNAL(ump)) {
        rc = journal_init(ump);
        if (rc != 0) {
            fprintf(stderr, "ufs-fuse: journal_init failed: %s\n",
                    strerror(-rc));
            ump->um_flags &= ~UFS_MOUNT_JOURNAL;
        }
    }

    return ump;
}

static void
do_unmount(struct ufs_mount *ump)
{
    if (ump == NULL)
        return;

    /* Flush journal and soft-updates */
    if (UFS_JOURNAL(ump) && ump->um_journal) {
        journal_commit(ump);
        journal_fini(ump);
    }
    if (UFS_SOFTDEP(ump) && ump->um_sdep)
        softdep_fini(ump);

    /* Flush buffer cache and write clean superblock */
    bdev_sync(ump->um_bdev);
    if (!UMP_RDONLY(ump))
        super_set_clean(ump, 1);

    inode_cache_fini(ump);
    bdev_close(ump->um_bdev);
    free(ump->um_fs);
    free(ump->um_csmem);
    free(ump);
}

/* ------------------------------------------------------------------ */
/* Option parsing                                                       */
/* ------------------------------------------------------------------ */

#define OPT_KEY_DEVICE  1001

static const struct fuse_opt ufs_fuse_opts_spec[] = {
    { "ro",           offsetof(struct ufs_fuse_opts, rdonly),     1 },
    { "softdep",      offsetof(struct ufs_fuse_opts, softdep),    1 },
    { "nosoftdep",    offsetof(struct ufs_fuse_opts, softdep),   -1 },
    { "journal",      offsetof(struct ufs_fuse_opts, journal),    1 },
    { "nojournal",    offsetof(struct ufs_fuse_opts, journal),   -1 },
    { "cache=%d",     offsetof(struct ufs_fuse_opts, cache_bufs), 0 },
    FUSE_OPT_END
};

static int
opt_proc(void *data, const char *arg, int key,
         struct fuse_args *outargs)
{
    struct ufs_fuse_opts *opts = data;

    if (key == FUSE_OPT_KEY_NONOPT) {
        if (opts->device == NULL) {
            opts->device = strdup(arg);
            return 0; /* consume — device is not a FUSE arg */
        }
        /* mountpoint: pass through to FUSE */
        return 1;
    }
    return 1; /* pass through */
}

/* ------------------------------------------------------------------ */
/* main                                                                 */
/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct ufs_fuse_opts opts;
    memset(&opts, 0, sizeof(opts));

    if (fuse_opt_parse(&args, &opts, ufs_fuse_opts_spec, opt_proc) != 0) {
        fprintf(stderr, "ufs-fuse: option parsing failed\n");
        return 1;
    }

    if (opts.device == NULL) {
        fprintf(stderr,
            "usage: ufs-fuse [options] <device|image> <mountpoint>\n"
            "  -o ro          read-only\n"
            "  -o softdep     enable soft updates\n"
            "  -o nosoftdep   disable soft updates\n"
            "  -o journal     enable soft-update journaling\n"
            "  -o nojournal   disable journaling\n"
            "  -o cache=N     buffer cache size (buffers, default 4096)\n"
        );
        fuse_opt_free_args(&args);
        return 1;
    }

    /*
     * Extract the mountpoint from the remaining args via FUSE3 API.
     */
    struct fuse_cmdline_opts fopts;
    memset(&fopts, 0, sizeof(fopts));
    if (fuse_parse_cmdline(&args, &fopts) != 0 ||
        fopts.mountpoint == NULL) {
        fprintf(stderr, "ufs-fuse: no mountpoint specified\n");
        fuse_opt_free_args(&args);
        free(opts.device);
        return 1;
    }
    char *mountpoint = fopts.mountpoint;
    int   foreground = fopts.foreground;
    int   mt         = !fopts.singlethread;

    struct ufs_mount *ump = do_mount(&opts);
    if (ump == NULL) {
        free(mountpoint);
        fuse_opt_free_args(&args);
        free(opts.device);
        return 1;
    }

    /* Register mount with FUSE ops */
    fuse_ops_set_mount(ump);

    /* Create FUSE session */
    struct fuse_session *se = fuse_session_new(
        &args, fuse_ufs_ops(),
        sizeof(*fuse_ufs_ops()), NULL);

    if (se == NULL) {
        fprintf(stderr, "ufs-fuse: fuse_session_new failed\n");
        do_unmount(ump);
        free(mountpoint);
        fuse_opt_free_args(&args);
        free(opts.device);
        return 1;
    }

    if (fuse_set_signal_handlers(se) != 0) {
        fprintf(stderr, "ufs-fuse: fuse_set_signal_handlers failed\n");
        fuse_session_destroy(se);
        do_unmount(ump);
        free(mountpoint);
        fuse_opt_free_args(&args);
        free(opts.device);
        return 1;
    }

    if (fuse_session_mount(se, mountpoint) != 0) {
        fprintf(stderr, "ufs-fuse: fuse_session_mount failed on %s\n",
                mountpoint);
        fuse_remove_signal_handlers(se);
        fuse_session_destroy(se);
        do_unmount(ump);
        free(mountpoint);
        fuse_opt_free_args(&args);
        free(opts.device);
        return 1;
    }

    fuse_daemonize(!foreground);

    /* Enter event loop (multi-threaded if mt > 0) */
    int rc;
    if (mt)
        rc = fuse_session_loop_mt(se, 0);
    else
        rc = fuse_session_loop(se);

    fuse_session_unmount(se);
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);

    do_unmount(ump);
    free(mountpoint);
    fuse_opt_free_args(&args);
    free(opts.device);

    return (rc == 0) ? 0 : 1;
}
