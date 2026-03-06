# LFS-FUSE — A Log-Structured Filesystem in FUSE

A fully working log-structured filesystem (LFS) implemented in C and mounted via FUSE3.
Built stage by stage from raw block I/O up to crash recovery.

---

## What is a Log-Structured Filesystem?

In a traditional filesystem, updates overwrite data in place. In an LFS, **every write goes to the end of a log** — old data is never overwritten. This makes writes fast and sequential, but requires garbage collection to reclaim space occupied by old (dead) versions of blocks.

---

## Project Structure

```
lfs-fuse/
├── lfs.img          # 4MB disk image (created by mkfs_lfs)
├── mount/           # FUSE mount point
├── lfs              # FUSE binary (built by make)
├── mkfs_lfs         # Format tool
└── src/
    ├── lfs.h        # Shared structs, constants, API declarations
    ├── lfs.c        # FUSE frontend (all filesystem operations)
    ├── disk.c       # Block-level read/write (pread/pwrite)
    ├── log.c        # Log append, checkpoint, crash recovery
    ├── inode.c      # Inode read/write/alloc
    ├── gc.c         # Garbage collector
    ├── mkfs_lfs.c   # Disk formatter
    └── Makefile
```

---

## Disk Layout

```
Block 0   — Superblock (magic, total_blocks, log_tail, commit_seq)
Block 1   — Inode map  (128 × uint32_t: inode_no → block number)
Block 2   — Commit block (crash recovery seal)
Block 3   — Root directory data
Block 4   — hello.txt data  (created by mkfs)
Block 5   — hello.txt inode (created by mkfs)
Block 6+  — Log (all writes go here, log_tail advances forward)
```

Each segment is 32 blocks (128 KB). The first block of every segment is a **segment summary** recording which inode owns each block — used by the garbage collector.

---

## Build & Run

```bash
cd ~/lfs-fuse/src

# Build everything
make clean && make all

# Format a fresh disk image
make format

# Mount (Terminal 1 — stays in foreground)
../lfs -f ../mount

# Unmount (Terminal 2)
fusermount3 -u ~/lfs-fuse/mount
```

---

## Stages

### Stage 1 — Disk Layer
Raw read/write of 4096-byte blocks to `lfs.img` using `pread`/`pwrite`.
Every higher layer goes through `disk_read()` / `disk_write()`.

### Stage 2 — Log + Superblock
Every write appends to the end of the log instead of overwriting old data.
The superblock (block 0) records `log_tail` — where the next write goes.
Survives remount: `log_tail` is reloaded from the superblock on mount.

### Stage 3 — Inodes + Directory
Files have **inodes** (metadata: type, size, block pointers).
An **inode map** (block 1) maps inode numbers to their current block in the log.
A root directory (inode 0) maps filenames to inode numbers.
Supports: `create`, `read`, `write`, `getattr`, `readdir`.

### Stage 4 — Garbage Collection
Because writes never overwrite, old versions of blocks accumulate as **dead blocks**.
GC scans the inode map to find live blocks, compacts them to the front of the log,
fixes all inode pointers, and rewinds `log_tail` — reclaiming hundreds of blocks at once.
Triggered automatically when free blocks fall below `GC_THRESHOLD` (100 blocks).

### Stage 5 — Multi-block Files
Each inode has 10 direct block pointers (`direct[0]` through `direct[9]`),
supporting files up to **40 KB**. Read and write work correctly across block boundaries.

### Stage 6 — File Deletion (`unlink`)
`rm file` calls `lfs_unlink` which:
1. Zeros the directory entry (slot marked free, `inode_no = 0`)
2. Sets `inode_map[ino] = 0` — all data blocks become dead for GC
3. Checkpoints to disk

Works at any directory depth (added in Stage 7).

### Stage 7 — Subdirectories (`mkdir`, `rmdir`)
`path_to_inode` now walks any depth by splitting the path on `/` and
calling `lookup_in_dir` at each component. All operations (create, read,
write, unlink) automatically work inside subdirectories.

`mkdir` allocates a new directory inode with a fresh empty data block.
`rmdir` refuses to remove non-empty directories (`-ENOTEMPTY`).
Deleted inode slots are reused by new entries.

### Stage 8 — Crash Recovery
If power cuts out mid-write, the filesystem recovers cleanly on remount.

**How it works:**

Every `log_checkpoint` writes in this order:
1. Inode map → block 1
2. Superblock (with incremented `commit_seq`) → block 0
3. **Commit block** (magic + seq + log_tail + imap CRC) → block 2  ← written last

On mount, `log_recover` reads the commit block and checks:
- `commit_magic == LFS_COMMIT_MAGIC`
- `commit_seq` matches superblock
- `imap_crc` matches the loaded inode map
- `log_tail` matches superblock

If all match → clean mount, no recovery needed.

If any mismatch → incomplete checkpoint detected:
1. Scan backwards from `log_tail` to find the last non-zero block → rewind `log_tail`
2. Scan all log blocks to find valid inodes → rebuild inode map
3. Write a fresh checkpoint to seal the recovered state

---

## Testing

```bash
M=~/lfs-fuse/mount

# Basic file operations
echo "hello" > $M/test.txt
cat $M/test.txt
rm $M/test.txt

# Multi-block file (Stage 5)
dd if=/dev/urandom bs=20000 count=1 > $M/big.txt
wc -c $M/big.txt          # should print 20000

# Subdirectories (Stage 7)
mkdir $M/docs
echo "note" > $M/docs/note.txt
cat $M/docs/note.txt
mkdir $M/a && mkdir $M/a/b && mkdir $M/a/b/c
echo "deep" > $M/a/b/c/deep.txt
cat $M/a/b/c/deep.txt

# Crash recovery (Stage 8)
echo "survive" > $M/persist.txt
pkill -9 -f "lfs -f"      # hard kill — no clean unmount
cd ~/lfs-fuse/src && ../lfs -f ../mount   # remount — recovery runs
cat ~/lfs-fuse/mount/persist.txt          # data should still be there

# GC (Stage 4) — watch Terminal 1 for "GC: starting..."
for i in $(seq 1 50); do echo "data" > $M/f$i.txt; done
```

---

## On-Disk Structures

| Struct | Size | Purpose |
|---|---|---|
| `lfs_superblock` | 4096 B | Magic, block count, log_tail, commit_seq |
| `lfs_inode` | 4096 B | Type, size, nlinks, 10 direct block pointers |
| `lfs_dirent` | 32 B | inode_no + 28-char name |
| `lfs_segment_summary` | 4096 B | Owns info for each block in a 32-block segment |
| `lfs_commit` | 4096 B | Crash recovery seal: magic, seq, tail, imap CRC |

---

## Limitations & Future Work

| Stage | Feature | Status |
|---|---|---|
| 9 | Indirect block pointers (files > 40 KB) | Not yet |
| — | Hard links (`nlinks > 1`) | Not yet |
| — | File permissions / timestamps | Not yet |
| — | Directory entries > 1 block (> 128 files per dir) | Not yet |
