# ufsbe — FUSE3 UFS1/UFS2 with B-epsilon tree

**ufsbe** (`fuse-ufsbe`) is a fully userspace UFS1/UFS2 filesystem for FreeBSD,
built on [FUSE3](https://github.com/libfuse/libfuse).  It reads and writes the
same on-disk format produced by `newfs(8)` — no conversion required — while
replacing two internal data structures with **B-epsilon trees** that
substantially improve write-heavy and random-I/O performance.

Tested on **FreeBSD 15.1-RELEASE** against a 30-minute fio endurance suite
(14 TiB of I/O, zero data errors).

---

## What it does

A standard UFS2 filesystem stores a file's block map in the inode itself: twelve
direct block pointers (`di_db[]`) and up to three levels of indirect blocks
(`di_ib[]`).  Looking up or allocating block _N_ of a large file requires
reading up to three extra disk blocks just to navigate the indirection tree.
Similarly, finding a directory entry requires a linear scan through
DIRBLKSIZ-aligned record blocks.

ufsbe replaces both of these with **B-epsilon trees** (Brodal & Fagerberg 1996,
Bender et al. 2007) — write-optimised search trees that buffer insertions at
internal nodes and flush them lazily downward in batches.  The result is:

| Workload | Improvement over standard UFS indirect blocks |
|---|---|
| Random 4k read IOPS | **+574 %** |
| Random 4k write IOPS | **+570 %** |
| `stat()` throughput (large dir) | **+42 %** |
| Sequential read bandwidth | **+5.5 %** |
| Sequential write bandwidth | −12 % (write-amplification cost at leaf splits) |

The on-disk linear `struct direct` directory format is **kept unchanged**, so
`fsck_ffs(8)` and the kernel UFS driver remain fully compatible.

---

## Features

- **UFS1 and UFS2** — byte-for-byte compatible with `newfs(8)` output
- **Soft Updates (softdep)** — asynchronous, dependency-ordered metadata writes
- **Soft Update Journaling (SUJ)** — small circular write-ahead log; allows
  mount-time recovery in seconds rather than a full `fsck` pass
- **B-epsilon block map** — per-inode `lbn → pbn` tree stored in `di_extb[0]`,
  replacing all direct and indirect block pointer chains
- **B-epsilon directory index** — per-directory `FNV1a-64(name) → ino` tree
  stored in `di_extb[1]`; linear format preserved for fsck compatibility
- **LRU buffer cache** with write-back, dirty tracking, and `B_BARRIER` ordering
- **Full POSIX VFS surface**: lookup, getattr, setattr, read, write, create,
  mkdir, rmdir, unlink, rename, link, symlink, readlink, readdir, fsync,
  statfs, open, release
- Mounts existing UFS2 images/partitions without any conversion step
- The kernel UFS driver can remount a ufsbe-written filesystem; `fsck_ffs`
  will fully recover it

---

## Requirements

| Dependency | How to get it |
|---|---|
| FreeBSD 15.x | — |
| `fusefs` kernel module | `kldload fusefs` (base system) |
| `fusefs-libs3` | `pkg install fusefs-libs3` |
| `libufs` | base system (part of `lib/libufs`) |
| `pkg-config` | `pkg install pkgconf` |

---

## Building from source

```sh
git clone https://github.com/example/ufs-fuse
cd ufs-fuse
make
```

The binary is called **`ufsbe`** after the patch applied by the port.
When building directly from the repository the default `TARGET` is still
`ufs-fuse`; rename it or pass `TARGET=ufsbe`:

```sh
make TARGET=ufsbe
```

---

## Quick start

```sh
# Load the FUSE kernel module (once per boot)
sudo kldload fusefs

# Create a fresh UFS2 image with soft updates
truncate -s 4g disk.img
sudo newfs -O 2 -U /dev/$(sudo mdconfig -a -t vnode -f disk.img)

# Mount it
sudo ufsbe /dev/md0 /mnt

# Use it
sudo cp -r /usr/share/doc /mnt/

# Unmount
sudo umount /mnt
```

---

## Mount options

```
ufsbe [-o options] <special> <mountpoint>
```

| Option | Effect |
|---|---|
| `ro` | Mount read-only |
| `softdep` | Enable soft updates (auto-detected from superblock) |
| `nosoftdep` | Disable soft updates |
| `journal` | Enable SUJ (auto-detected from superblock) |
| `nojournal` | Disable SUJ |
| `allow_other` | Allow other users to access the mount |
| `default_permissions` | Enable kernel-side permission checks |

Soft updates and journaling are **auto-detected** from the superblock flags
written by `newfs -U` / `newfs -j`; you do not need to pass them explicitly.

---

## Mounting via mount(8) and fstab(5)

When installed as a port, `mount_ufsbe(8)` is placed in `/usr/local/sbin` and
`mount(8)` can invoke it automatically:

```sh
# Via mount(8)
mount -t ufsbe /dev/ada0s1a /mnt

# Via mount_fusefs(8)
mount_fusefs auto /mnt ufsbe /dev/ada0s1a
```

`/etc/fstab` entry:

```
/dev/ada0s1a  /mnt  ufsbe  rw  0  0
```

> **Note:** FUSE filesystems cannot be mounted during early boot (before the
> kernel module and daemon are available).  Use `late_mounts` in `rc.conf(5)`
> or a local `rc.d` script.

---

## How the B-epsilon tree works

A **B-epsilon tree** is a balanced search tree where every internal node holds
a small *insert buffer* in addition to its pivot keys and child pointers.
Insertions are written to the root's buffer first.  When a buffer is full,
its messages are flushed in a batch down to the appropriate child — amortising
the per-write I/O cost across many operations.  Lookups walk root-to-leaf,
accumulating any buffered messages for the target key along the way.

```
Internal node layout (one FS block)
┌──────────────────────────────────────┐
│ header (16 B)                        │
├──────────────────────────────────────┤
│ insert buffer  (¼ of usable space)   │  ← INSERT / DELETE messages queue here
│  [op | key | value] × N              │
├──────────────────────────────────────┤
│ pivot keys     (¾ of usable space)   │
│ child block numbers                  │
└──────────────────────────────────────┘

Leaf node layout
┌──────────────────────────────────────┐
│ header (16 B)                        │
├──────────────────────────────────────┤
│ sorted key / value pairs             │  ← actual data lives here
└──────────────────────────────────────┘
```

For a 4096-byte block: up to **63 buffered messages** per internal node and up
to **191 key/value pairs** per leaf.  Node I/O goes through the existing LRU
buffer cache (`buf_bread` / `bdwrite`), so hot nodes stay in memory.

### Block map tree

Key = logical block number (LBN), value = physical FS block number (PBN).
Stored in `di_extb[0]` of the on-disk `ufs2_dinode`.

### Directory index tree

Key = `FNV1a-64(name) XOR (namelen << 32)`, value = inode number.
Stored in `di_extb[1]`.  The full linear `struct direct` scan is still used
for `readdir` and `isempty` (which must enumerate all entries), and as a
fallback for the rare 64-bit hash collision.

---

## Architecture

```
  ┌─────────────────────────────────────────────────────────┐
  │  FUSE3 low-level API          fuse_ops.c / main.c        │
  └───────────────────────────┬─────────────────────────────┘
                              │ VFS calls
  ┌───────────────────────────▼─────────────────────────────┐
  │  Inode layer              inode.c                        │
  │  • bmap via B-ε tree      betree.c                       │
  │  • read / write / trunc                                  │
  ├─────────────────────────────────────────────────────────┤
  │  Directory layer          dir.c                          │
  │  • lookup via B-ε index   betree.c                       │
  │  • linear format on disk  (struct direct)                │
  ├──────────────────┬──────────────────────────────────────┤
  │  Soft Updates    │  SUJ Journal                          │
  │  softdep.c       │  journal.c (circular log, CRC32)      │
  ├──────────────────┴──────────────────────────────────────┤
  │  Buffer cache             block.c                        │
  │  • LRU eviction, B_BARRIER ordering                      │
  │  • buf_bread / bdwrite / brelse                          │
  ├─────────────────────────────────────────────────────────┤
  │  Superblock + cylinder groups   super.c                  │
  │  • libufs (sbget/sbput/cgget/cgput)                      │
  │  • block & inode allocation / free                       │
  ├─────────────────────────────────────────────────────────┤
  │  pread / pwrite                                          │
  │  disk image or block device                              │
  └─────────────────────────────────────────────────────────┘
```

---

## Source layout

| File | Description |
|---|---|
| `src/main.c` | Argument parsing, mount, FUSE session loop |
| `src/fuse_ops.c/.h` | FUSE3 low-level operation callbacks; ino translation |
| `src/inode.c/.h` | Inode cache, bmap (B-ε), read/write, truncate, alloc |
| `src/dir.c/.h` | Directory ops: lookup (B-ε fast-path), add/remove, readdir |
| `src/betree.c/.h` | Persistent B-epsilon tree: lookup, insert, delete, range |
| `src/block.c/.h` | LRU buffer cache; `buf_bread`, `bdwrite`, `brelse`, `bdirty` |
| `src/super.c/.h` | Superblock, cylinder group, block/inode allocator |
| `src/softdep.c/.h` | Soft-updates dependency engine |
| `src/journal.c/.h` | SUJ circular write-ahead log, CRC32, replay |
| `src/ufs_fs.h` | Wrapper: `<ufs/ffs/fs.h>` + UFS macros |
| `src/ufs_dinode.h` | Wrapper: `<ufs/ufs/dinode.h>` |
| `src/ufs_dir.h` | Wrapper: `<ufs/ufs/dir.h>` |
| `src/ufs_inode.h` | In-core inode (`struct inode`, B-ε root fields) |
| `src/ufs_block.h` | In-core buffer (`struct buf`, B_* flags) |
| `src/ufs_mount.h` | In-core mount state (`struct ufs_mount`) |

---

## Port installation (fuse-ufsbe)

The `port/` directory contains a complete FreeBSD port.
The package name is **`fuse-ufsbe`**; the installed commands are **`ufsbe`**
and **`mount_ufsbe`**.

### From the ports tree

```sh
# Copy the port into the ports tree
cp -r port /usr/ports/filesystems/fuse-ufsbe

# Build and install
cd /usr/ports/filesystems/fuse-ufsbe
make install clean
```

### What gets installed

| Path | Purpose |
|---|---|
| `/usr/local/sbin/ufsbe` | FUSE3 UFS daemon |
| `/usr/local/sbin/mount_ufsbe` | Wrapper for `mount(8)` / `fstab(5)` |
| `/usr/local/share/man/man8/ufsbe.8.gz` | `ufsbe(8)` man page |
| `/usr/local/share/man/man8/mount_ufsbe.8.gz` | `mount_ufsbe(8)` man page |

### Hosting your own tarball

Update `MASTER_SITES` in [`port/Makefile`](port/Makefile) to point at your
server, then regenerate `port/distinfo`:

```sh
cd port
make makesum DISTDIR=/path/to/your/tarballs
```

---

## Benchmark results

All runs on FreeBSD 15.1, 10 GiB vnode-backed md device, UFS2 with soft updates.

### 30-minute endurance (v4 / B-epsilon)

| Metric | Result |
|---|---|
| Total I/O | **14,761 GiB** |
| Data errors | **0** |
| ENOSPC events | 84 (disk-full write retries — benign) |

### master (indirect-ptr) vs v4 (B-epsilon) — 120 s per workload

| Workload | master | v4 (B-ε) | Δ |
|---|---|---|---|
| Seq write 128k BW | 3.6 MiB/s | 3.2 MiB/s | −12 % |
| Seq read 128k BW | 1,898 MiB/s | 2,002 MiB/s | **+6 %** |
| Random 4k read IOPS | 160 | 1,081 | **+574 %** |
| Random 4k write IOPS | 107 | 720 | **+570 %** |
| Random 4k write latency | 12,267 µs | 6,104 µs | **−50 %** |
| `stat()` throughput | 444 /s | 631 /s | **+42 %** |

To reproduce:

```sh
sudo sh bench/compare.sh 120
```

---

## Git branches

| Branch | Description |
|---|---|
| `master` | Traditional UFS direct/indirect block map, linear directory scan |
| `v3` | B-epsilon block map + directory index (first working implementation) |
| `v4` | Current development branch; includes port infrastructure |

---

## Caveats

- Hard links, extended attributes, and ACLs are not currently supported.
- Triple-indirect block addresses are not handled in the B-epsilon path; the
  practical file-size limit is `(nindir² + nindir + 12) × bsize` (≈ 512 GiB
  on a 4096-byte block filesystem).
- The B-epsilon roots live in `di_extb[0..1]`.  If extended attributes are
  later enabled on the same filesystem by the kernel, those fields conflict.
- `allow_other` requires either root or `sysctl vfs.usermount=1`.

---

## License

BSD 2-Clause — matching the FreeBSD kernel sources this is derived from.
