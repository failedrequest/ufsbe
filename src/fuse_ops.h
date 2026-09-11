/*
 * fuse_ops.h — FUSE3 low-level operation table.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef FUSE_OPS_H
#define FUSE_OPS_H

#define FUSE_USE_VERSION 35
#include <fuse_lowlevel.h>
#include "ufs_mount.h"

/* Returns the global fuse_lowlevel_ops table. */
const struct fuse_lowlevel_ops *fuse_ufs_ops(void);

/* Called once at mount time to set the mount context for op callbacks. */
void fuse_ops_set_mount(struct ufs_mount *ump);

#endif /* FUSE_OPS_H */
