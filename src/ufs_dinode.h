/*
 * ufs_dinode.h — Thin wrapper: use system on-disk inode headers directly.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UFS_DINODE_H
#define UFS_DINODE_H

#include <sys/types.h>
#include <ufs/ufs/dinode.h>

/*
 * The system header defines:
 *   struct ufs1_dinode  (128 bytes)
 *   struct ufs2_dinode  (256 bytes)
 *   UFS_NDADDR, UFS_NIADDR
 *   ufs1_daddr_t (int32_t), ufs2_daddr_t (int64_t), ufs_time_t (int64_t)
 *   ROOTINO, WINO
 *   IFMT, IFREG, IFDIR, IFLNK, IFCHR, IFBLK, IFIFO, IFSOCK, IFWHT
 *   IREAD, IWRITE, IEXEC, ISUID, ISGID, ISVTX
 *   UF_*, SF_* flags
 *   UFS1_MAXSYMLINKLEN, UFS2_MAXSYMLINKLEN
 */

/* Compatibility aliases — our code uses NDADDR/NIADDR/ROOTINO */
#ifndef NDADDR
#define NDADDR  UFS_NDADDR
#define NIADDR  UFS_NIADDR
#endif

#ifndef ROOTINO
#define ROOTINO ((uint32_t)UFS_ROOTINO)
#endif

#endif /* UFS_DINODE_H */
