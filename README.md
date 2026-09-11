# ufs-fuse

A FUSE3 userspace implementation of FreeBSD UFS1/UFS2 with full **soft updates**
and **soft-update journaling (SUJ)** support, derived from the FreeBSD 15.1 kernel
sources (`/usr/src/sys/ufs/`).

## Features

- **UFS1 and UFS2** on-disk format, byte-for-byte compatible with FreeBSD `newfs`
- **Soft updates** — asynchronous, dependency-ordered metadata writes that keep
  the filesystem consistent without journaling overhead
- **Soft-update journaling (SUJ)** — a small write-ahead log layered on top of
  soft updates; allows recovery in seconds rather than running a full `fsck`
- Full POSIX VFS: `lookup`, `getattr`, `setattr`, `read`, `write`, `create`,
  `mkdir`, `rmdir`, `unlink`, `rename`, `link`, `symlink`, `readlink`,
  `readdir`, `fsync`, `statfs`, `open`, `release`
- Block/buffer cache with LRU eviction and dirty-page write-back
- Cylinder-group allocation with best-fit fragment management

## Requirements

- FreeBSD 15.x (or compatible)
- `fusefs-libs3` (`pkg install fusefs-libs3`)
- A UFS1/UFS2 image or device created with `newfs`

## Building

```sh
make
```

## Usage

```sh
# Mount a UFS2 image (soft updates, no journaling)
./ufs-fuse -o softdep /path/to/image /mnt/point

# Mount with soft-update journaling
./ufs-fuse -o softdep,journal /path/to/image /mnt/point

# Read-only mount
./ufs-fuse -o ro /path/to/image /mnt/point

# Foreground / debug
./ufs-fuse -f -d /path/to/image /mnt/point
```

## Architecture

```
FUSE3 low-level API  (fuse_ops.c / main.c)
        │
        ▼
VFS layer  (inode.c · dir.c)
        │
   ┌────┴────┐
   │ SoftDep │   Journal (journal.c)
   │(softdep)│◄──write-ahead log / recovery
   └────┬────┘
        │
Block/Buffer Cache  (block.c)
        │
Superblock + Cylinder Groups  (super.c)
        │
  pread / pwrite
        │
   disk / image
```

## Source layout

| File | Description |
|------|-------------|
| `src/ufs_fs.h` | On-disk superblock, cylinder group, macros (UFS1/UFS2) |
| `src/ufs_dinode.h` | On-disk inode formats (ufs1_dinode, ufs2_dinode) |
| `src/ufs_dir.h` | On-disk directory entry structures |
| `src/ufs_inode.h` | In-core inode |
| `src/ufs_mount.h` | In-core mount state |
| `src/block.h/.c` | Block I/O and buffer cache |
| `src/super.h/.c` | Superblock and cylinder-group management |
| `src/inode.h/.c` | Inode allocation, bmap, truncate, read/write |
| `src/dir.h/.c` | Directory operations |
| `src/softdep.h/.c` | Soft-updates dependency engine |
| `src/journal.h/.c` | SUJ write-ahead journal and recovery |
| `src/fuse_ops.h/.c` | FUSE3 low-level operation callbacks |
| `src/main.c` | Argument parsing, mount, event loop |

## License

BSD 2-Clause (matching FreeBSD kernel sources this is derived from).
