/*
 * softdep.c — Soft-updates dependency engine implementation.
 *
 * Overview of the algorithm (McKusick & Ganger, 1999):
 *
 * Every metadata write that could leave the filesystem inconsistent on
 * crash is accompanied by a dependency record.  Writes are not blocked
 * outright; instead:
 *
 *   - The in-memory inode or block may be written to disk at any time,
 *     but the written image is a "safe" version that omits changes that
 *     would create a dangerous forward reference.
 *
 *   - When a buffer completes I/O, its attached dependency list is
 *     walked; satisfied dependencies are removed; newly unblocked
 *     dependencies trigger deferred work (e.g. actually writing a
 *     directory entry's ino field after the inode is on disk).
 *
 *   - On fsync() we drain all pending dependencies for that inode.
 *
 * In this userspace port:
 *   - "Write" means pushing a buf to the bdev (bwrite / bdwrite).
 *   - "I/O completion" is synchronous after bwrite; bdwrite completion
 *     is signalled by bdev_sync() or softdep_flush().
 *   - The lock ss_lock serialises all dep-list manipulation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>

#include "softdep.h"
#include "block.h"
#include "inode.h"
#include "super.h"
#include "ufs_mount.h"
#include "ufs_fs.h"

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static inline struct softdep_state *
sd_state(struct ufs_mount *ump)
{
    return (struct softdep_state *)ump->um_sdep;
}

static inline uint32_t
ino_hash(uint32_t ino)
{
    return (ino ^ (ino >> 4)) & 255u;
}

/* Allocate and zero a dependency node of the given size. */
static struct sd_dep *
sd_alloc(sd_kind_t kind, size_t sz)
{
    struct sd_dep *dep = calloc(1, sz);
    if (dep == NULL)
        return NULL;
    dep->sd_kind = kind;
    LIST_INIT(&dep->sd_dependents);
    return dep;
}

#define SD_ALLOC(kind, type) \
    ((struct type *)sd_alloc(kind, sizeof(struct type)))

/* Link dep as a blocker of parent (parent waits for dep to complete). */
static void
sd_depend(struct sd_dep *parent, struct sd_dep *dep)
{
    dep->sd_parent = parent;
    LIST_INSERT_HEAD(&parent->sd_dependents, dep, sd_dep_link);
}

/* Check whether dep has all its own dependents satisfied. */
static int
sd_complete(struct sd_dep *dep)
{
    return LIST_EMPTY(&dep->sd_dependents);
}

/* Remove dep from whatever lists it is on and free it. */
static void
sd_free(struct softdep_state *ss, struct sd_dep *dep)
{
    if (dep->sd_flags & SD_ONWORKLIST)
        TAILQ_REMOVE(&ss->ss_worklist, dep, sd_worklist);
    if (dep->sd_parent != NULL)
        LIST_REMOVE(dep, sd_dep_link);
    ss->ss_ndeps--;
    free(dep);
}

/* ------------------------------------------------------------------ */
/* Init / fini                                                          */
/* ------------------------------------------------------------------ */

int
softdep_init(struct ufs_mount *ump)
{
    struct softdep_state *ss = calloc(1, sizeof(*ss));
    if (ss == NULL)
        return -ENOMEM;

    TAILQ_INIT(&ss->ss_worklist);
    pthread_mutex_init(&ss->ss_lock, NULL);

    for (int i = 0; i < 256; i++)
        LIST_INIT(&ss->ss_inohash[i]);
    for (int i = 0; i < 512; i++)
        LIST_INIT(&ss->ss_blkhash[i]);

    ump->um_sdep = ss;
    return 0;
}

void
softdep_fini(struct ufs_mount *ump)
{
    softdep_flush(ump);

    struct softdep_state *ss = sd_state(ump);
    struct sd_dep *dep, *tmp;

    pthread_mutex_lock(&ss->ss_lock);
    TAILQ_FOREACH_SAFE(dep, &ss->ss_worklist, sd_worklist, tmp) {
        TAILQ_REMOVE(&ss->ss_worklist, dep, sd_worklist);
        free(dep);
    }
    pthread_mutex_unlock(&ss->ss_lock);

    pthread_mutex_destroy(&ss->ss_lock);
    free(ss);
    ump->um_sdep = NULL;
}

/* ------------------------------------------------------------------ */
/* Flush: drain the worklist                                            */
/* ------------------------------------------------------------------ */

/*
 * Process a single work item.  The rules:
 *
 *   SD_INODEDEP:  If all dependents (diradd records) are complete, write
 *                 the inode (with its real nlink) to disk.
 *   SD_DIRADD:    If the inode is on disk (its inodedep is complete),
 *                 write the directory block with the real ino field.
 *   SD_DIRREM:    After the inode's nlink is on disk, write the freed
 *                 block list.
 *   SD_FREEBLKS:  If all pointers to these blocks are removed, free them.
 */
static void
sd_process_one(struct ufs_mount *ump, struct sd_dep *dep)
{
    struct softdep_state *ss = sd_state(ump);

    switch (dep->sd_kind) {

    case SD_INODEDEP: {
        struct sd_inodedep *id = (struct sd_inodedep *)dep;
        if (!sd_complete(dep))
            break;
        dep->sd_flags |= SD_DEPCOMPLETE;
        /* Write the real inode */
        struct inode *ip = inode_get(ump, id->id_ino);
        if (ip != NULL) {
            inode_update(ump, ip, 1);
            inode_put(ump, ip);
        }
        sd_free(ss, dep);
        break;
    }

    case SD_DIRADD: {
        struct sd_diradd *da = (struct sd_diradd *)dep;
        /* Check if the inodedep is complete */
        uint32_t h = ino_hash(da->da_ino);
        struct sd_dep *id_dep;
        int inode_safe = 1;
        LIST_FOREACH(id_dep, &ss->ss_inohash[h], sd_hash) {
            if (id_dep->sd_kind == SD_INODEDEP) {
                struct sd_inodedep *id = (struct sd_inodedep *)id_dep;
                if (id->id_ino == da->da_ino) {
                    inode_safe = (id->id_dep.sd_flags & SD_DEPCOMPLETE) != 0;
                    break;
                }
            }
        }
        if (!inode_safe)
            break;
        dep->sd_flags |= SD_DEPCOMPLETE;
        sd_free(ss, dep);
        break;
    }

    case SD_DIRREM: {
        struct sd_dirrem *dr = (struct sd_dirrem *)dep;
        /* Once the associated inodedep is off disk we can proceed */
        uint32_t h = ino_hash(dr->dr_ino);
        struct sd_dep *id_dep;
        LIST_FOREACH(id_dep, &ss->ss_inohash[h], sd_hash) {
            if (id_dep->sd_kind == SD_INODEDEP) {
                struct sd_inodedep *id = (struct sd_inodedep *)id_dep;
                if (id->id_ino == dr->dr_ino)
                    goto dirrem_blocked;
            }
        }
        dep->sd_flags |= SD_DEPCOMPLETE;
        sd_free(ss, dep);
        break;
    dirrem_blocked:
        break;
    }

    case SD_FREEBLKS: {
        struct sd_freeblks *fb = (struct sd_freeblks *)dep;
        if (!sd_complete(dep))
            break;
        dep->sd_flags |= SD_DEPCOMPLETE;
        for (int i = 0; i < fb->fb_nblks; i++) {
            if (fb->fb_blknos[i] > 0)
                fs_free_block(ump, fb->fb_blknos[i], fb->fb_bsize);
        }
        sd_free(ss, dep);
        break;
    }

    default:
        sd_free(ss, dep);
        break;
    }
}

int
softdep_flush(struct ufs_mount *ump)
{
    struct softdep_state *ss = sd_state(ump);
    int passes = 0;

    /* Flush the buffer cache first so I/O completions fire */
    bdev_sync(ump->um_bdev);
    super_flush(ump);

    pthread_mutex_lock(&ss->ss_lock);
    while (!TAILQ_EMPTY(&ss->ss_worklist) && passes < 32) {
        /* Snapshot the list to avoid iterator invalidation */
        struct sd_dep *dep, *tmp;
        TAILQ_FOREACH_SAFE(dep, &ss->ss_worklist, sd_worklist, tmp) {
            sd_process_one(ump, dep);
        }
        passes++;
    }
    pthread_mutex_unlock(&ss->ss_lock);

    bdev_sync(ump->um_bdev);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Setup hooks called by inode/dir layers                               */
/* ------------------------------------------------------------------ */

void
softdep_setup_inode_alloc(struct ufs_mount *ump, struct inode *ip,
                           uint16_t mode)
{
    (void)mode;
    struct softdep_state *ss = sd_state(ump);
    struct sd_inodedep   *id = SD_ALLOC(SD_INODEDEP, sd_inodedep);
    if (id == NULL)
        return;

    id->id_ino = ip->i_number;

    /* Save a "safe" inode image (zeroed out nlink) for deferred writes */
    size_t disz = UFS_IS2(ump) ? sizeof(struct ufs2_dinode) :
                                  sizeof(struct ufs1_dinode);
    id->id_savedino = malloc(disz);
    if (id->id_savedino != NULL) {
        void *src = UFS_IS2(ump) ?
            (void *)&ip->i_din.di2 : (void *)&ip->i_din.di1;
        memcpy(id->id_savedino, src, disz);
        /* Safe copy has nlink=0 */
        if (UFS_IS2(ump))
            ((struct ufs2_dinode *)id->id_savedino)->di_nlink = 0;
        else
            ((struct ufs1_dinode *)id->id_savedino)->di_nlink = 0;
    }

    pthread_mutex_lock(&ss->ss_lock);
    LIST_INSERT_HEAD(&ss->ss_inohash[ino_hash(ip->i_number)],
                     &id->id_dep, sd_hash);
    TAILQ_INSERT_TAIL(&ss->ss_worklist, &id->id_dep, sd_worklist);
    id->id_dep.sd_flags |= SD_ONWORKLIST | SD_ATTACHED;
    ss->ss_ndeps++;
    pthread_mutex_unlock(&ss->ss_lock);
}

void
softdep_setup_diradd(struct ufs_mount *ump,
                      struct inode *dp, struct inode *ip,
                      const char *name, size_t namelen,
                      off_t offset, uint8_t dtype)
{
    struct softdep_state *ss = sd_state(ump);
    struct sd_diradd     *da = SD_ALLOC(SD_DIRADD, sd_diradd);
    if (da == NULL)
        return;

    da->da_ino    = ip->i_number;
    da->da_dirino = dp->i_number;
    da->da_offset = offset;
    da->da_type   = dtype;
    da->da_namlen = (uint8_t)(namelen > 255 ? 255 : namelen);
    memcpy(da->da_name, name, da->da_namlen);

    /* Find the inodedep for this inode and attach */
    pthread_mutex_lock(&ss->ss_lock);
    uint32_t h = ino_hash(ip->i_number);
    struct sd_dep *id_dep;
    LIST_FOREACH(id_dep, &ss->ss_inohash[h], sd_hash) {
        if (id_dep->sd_kind == SD_INODEDEP) {
            struct sd_inodedep *id = (struct sd_inodedep *)id_dep;
            if (id->id_ino == ip->i_number) {
                sd_depend(id_dep, &da->da_dep);
                break;
            }
        }
    }

    TAILQ_INSERT_TAIL(&ss->ss_worklist, &da->da_dep, sd_worklist);
    da->da_dep.sd_flags |= SD_ONWORKLIST | SD_ATTACHED;
    ss->ss_ndeps++;
    pthread_mutex_unlock(&ss->ss_lock);
}

void
softdep_setup_dirrem(struct ufs_mount *ump,
                      struct inode *dp, struct inode *ip,
                      off_t offset)
{
    struct softdep_state *ss = sd_state(ump);
    struct sd_dirrem     *dr = SD_ALLOC(SD_DIRREM, sd_dirrem);
    if (dr == NULL)
        return;

    dr->dr_ino    = ip->i_number;
    dr->dr_dirino = dp->i_number;
    dr->dr_offset = offset;

    pthread_mutex_lock(&ss->ss_lock);
    TAILQ_INSERT_TAIL(&ss->ss_worklist, &dr->dr_dep, sd_worklist);
    dr->dr_dep.sd_flags |= SD_ONWORKLIST | SD_ATTACHED;
    ss->ss_ndeps++;
    pthread_mutex_unlock(&ss->ss_lock);
}

void
softdep_setup_freeblks(struct ufs_mount *ump, struct inode *ip,
                        int64_t *blknos, int nblks, int bsize)
{
    struct softdep_state *ss = sd_state(ump);
    struct sd_freeblks   *fb = SD_ALLOC(SD_FREEBLKS, sd_freeblks);
    if (fb == NULL)
        return;

    fb->fb_ino   = ip->i_number;
    fb->fb_bsize = bsize;
    fb->fb_nblks = (nblks > 16) ? 16 : nblks;
    memcpy(fb->fb_blknos, blknos, (size_t)fb->fb_nblks * sizeof(int64_t));

    /* Attach to inodedep if one exists */
    pthread_mutex_lock(&ss->ss_lock);
    uint32_t h = ino_hash(ip->i_number);
    struct sd_dep *id_dep;
    LIST_FOREACH(id_dep, &ss->ss_inohash[h], sd_hash) {
        if (id_dep->sd_kind == SD_INODEDEP) {
            struct sd_inodedep *id = (struct sd_inodedep *)id_dep;
            if (id->id_ino == ip->i_number) {
                sd_depend(id_dep, &fb->fb_dep);
                break;
            }
        }
    }

    TAILQ_INSERT_TAIL(&ss->ss_worklist, &fb->fb_dep, sd_worklist);
    fb->fb_dep.sd_flags |= SD_ONWORKLIST | SD_ATTACHED;
    ss->ss_ndeps++;
    pthread_mutex_unlock(&ss->ss_lock);
}

void
softdep_setup_mkdir(struct ufs_mount *ump,
                     struct inode *dp, struct inode *newip)
{
    /* mkdir requires both the new directory's inode and the parent
     * directory entry to be safely on disk.  We model this by ensuring
     * the newip inodedep is resolved before the diradd. */
    softdep_setup_inode_alloc(ump, newip, inode_mode(ump, newip));
    /* The diradd is set up by the caller via softdep_setup_diradd */
    (void)dp;
}

void
softdep_buf_written(struct ufs_mount *ump, struct buf *bp)
{
    /* In the userspace model we process the worklist on every sync.
     * A real implementation would walk deps attached to bp here. */
    (void)bp;
    struct softdep_state *ss = sd_state(ump);
    struct sd_dep *dep, *tmp;

    pthread_mutex_lock(&ss->ss_lock);
    TAILQ_FOREACH_SAFE(dep, &ss->ss_worklist, sd_worklist, tmp) {
        if (dep->sd_flags & SD_DEPCOMPLETE)
            sd_free(ss, dep);
    }
    pthread_mutex_unlock(&ss->ss_lock);
}

int
softdep_fsync(struct ufs_mount *ump, struct inode *ip)
{
    /* Drain all dependencies for this inode */
    struct softdep_state *ss = sd_state(ump);
    struct sd_dep *dep, *tmp;

    pthread_mutex_lock(&ss->ss_lock);
    TAILQ_FOREACH_SAFE(dep, &ss->ss_worklist, sd_worklist, tmp) {
        int relevant = 0;
        switch (dep->sd_kind) {
        case SD_INODEDEP:
            relevant = (((struct sd_inodedep *)dep)->id_ino == ip->i_number);
            break;
        case SD_DIRADD:
            relevant = (((struct sd_diradd *)dep)->da_dirino == ip->i_number ||
                        ((struct sd_diradd *)dep)->da_ino == ip->i_number);
            break;
        case SD_DIRREM:
            relevant = (((struct sd_dirrem *)dep)->dr_dirino == ip->i_number);
            break;
        case SD_FREEBLKS:
            relevant = (((struct sd_freeblks *)dep)->fb_ino == ip->i_number);
            break;
        default:
            break;
        }
        if (relevant)
            sd_process_one(ump, dep);
    }
    pthread_mutex_unlock(&ss->ss_lock);

    /* Write out any dirty buffers */
    bdev_sync(ump->um_bdev);
    inode_update(ump, ip, 1);
    return 0;
}

int
softdep_inode_defer(struct ufs_mount *ump, struct inode *ip)
{
    struct softdep_state *ss = sd_state(ump);
    uint32_t h = ino_hash(ip->i_number);
    struct sd_dep *dep;

    pthread_mutex_lock(&ss->ss_lock);
    LIST_FOREACH(dep, &ss->ss_inohash[h], sd_hash) {
        if (dep->sd_kind == SD_INODEDEP) {
            struct sd_inodedep *id = (struct sd_inodedep *)dep;
            if (id->id_ino == ip->i_number &&
                !(dep->sd_flags & SD_DEPCOMPLETE)) {
                pthread_mutex_unlock(&ss->ss_lock);
                return 1;
            }
        }
    }
    pthread_mutex_unlock(&ss->ss_lock);
    return 0;
}
