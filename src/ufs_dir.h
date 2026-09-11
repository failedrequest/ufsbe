/*
 * ufs_dir.h — Thin wrapper: use system UFS directory header directly.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef UFS_DIR_H
#define UFS_DIR_H

#include <sys/types.h>
#include <ufs/ufs/dir.h>

/*
 * The system header defines:
 *   struct direct { d_ino, d_reclen, d_type, d_namlen, d_name[] }
 *   DIRBLKSIZ, MAXNAMLEN
 *   DT_UNKNOWN, DT_FIFO, DT_CHR, DT_DIR, DT_BLK, DT_REG, DT_LNK, DT_SOCK, DT_WHT
 *   IFTODT(mode), DTTOIF(dirtype)
 *   DIRSIZ(oldfmt, dp)
 *   struct dirtemplate, struct odirtemplate
 */

/* DIRECTSIZ: minimum record length for a name of namlen bytes.
 * The system header uses DIRSIZ; we provide DIRECTSIZ as an alias. */
#ifndef DIRECTSIZ
#include <stddef.h>
#define DIRECTSIZ(namlen) \
    (((int)(offsetof(struct direct, d_name)) + ((namlen) + 1) + 3) & ~3)
#endif

/* OLDDIRFMT / NEWDIRFMT may not be in the system header */
#ifndef OLDDIRFMT
#define OLDDIRFMT 1
#define NEWDIRFMT 0
#endif

/* Compatibility alias: older code uses MAXNAMLEN */
#ifndef MAXNAMLEN
#define MAXNAMLEN UFS_MAXNAMLEN
#endif

#endif /* UFS_DIR_H */
