# LFS-FUSE — A Log-Structured Filesystem in FUSE

A fully working log-structured filesystem (LFS) implemented in C and mounted via FUSE3.
Built stage by stage from raw block I/O up to crash recovery and indirect block pointers.

---

## What is a Log-Structured Filesystem?

In a traditional filesystem, updates overwrite data in place. In an LFS, **every write goes to the end of a log** — old data is never overwritten. This makes writes fast and sequential, but requires garbage collection to reclaim space occupied by old (dead) versions of blocks.

This is the same principle used in **SSDs and flash storage** (which cannot overwrite in place), **database write-ahead logs** (PostgreSQL, SQLite), and **journaling filesystems** (ext4, APFS).

---

## Project Structure

```
lfs-fuse/
├── .gitignore
├── README.md
├── lfs              # FUSE binary (built by make, gitignored)
├── mkfs_lfs         # Format tool (built by make, gitignored)
├── lfs.img          # 4MB disk image (created by mkfs_lfs, gitignored)
├── mount/           # FUSE mount point (gitignored)
└── src/
    ├── Makefile
    ├── lfs.h        # Shared structs, constants, API declarations
    ├── lfs.c        # FUSE frontend — all filesystem operations
    ├── disk.c       # Block-level read/write (pread/pwrite)
    ├── log.c        # Log append, checkpoint, crash recovery
    ├── inode.c      # Inode read/write/alloc
    ├── gc.c         # Garbage collector
    └── mkfs_lfs.c   # Disk formatter
```

---

## Disk Layout

```
Block 0   — Superblock       (magic, total_blocks, log_tail, commit_seq)
Block 1   — Inode map        (128 × uint32_t: inode_no → block number)
Block 2   — Commit block     (crash recovery seal: magic, seq, crc)
Block 3   — Root inode       (inode 0, created by mkfs)
Block 4   — Root dir data    (hello.txt dirent, created by mkfs)
Block 5   — hello.txt data   (created by mkfs)
Block 6   — hello.txt inode  (inode 1, created by mkfs)
Block 7+  — Log              (all writes go here, log_tail advances forward)
```

Each segment is 32 blocks (128 KB). The first block of every segment is a **segment summary** recording which inode owns each block — used by the garbage collector to distinguish live from dead blocks.

---

## Build & Run

**Two terminals are recommended** — one for the filesystem process, one for testing.

```bash
# Terminal 1 — build and mount
cd ~/lfs-fuse/src
make clean && make all    # compile everything
make format               # write a fresh lfs.img
../lfs -f ../mount        # mount (stays in foreground, prints debug output)

# Terminal 2 — unmount cleanly when done
fusermount3 -u ~/lfs-fuse/mount
```

To remount existing data without reformatting (keeps your files):
```bash
cd ~/lfs-fuse/src && ../lfs -f ../mount
```

---

## Usage

Once mounted, the filesystem works like any normal Linux directory:

```bash
M=~/lfs-fuse/mount          # shorthand — M is just a variable

# Files
echo "hello" > $M/file.txt
cat $M/file.txt
ls $M
rm $M/file.txt

# Directories
mkdir $M/docs
echo "note" > $M/docs/note.txt
cat $M/docs/note.txt
rmdir $M/docs               # only works if empty

# Deep nesting
mkdir -p $M/a/b/c
echo "deep" > $M/a/b/c/deep.txt

# Copy files in/out
cp ~/somefile.txt $M/
cp $M/file.txt ~/

# Large files (up to ~4MB)
dd if=/dev/urandom of=$M/big.bin bs=1048576 count=1
wc -c $M/big.bin            # 1048576
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
fixes all inode and indirect pointers, and rewinds `log_tail` — reclaiming hundreds
of blocks at once. Triggered automatically when free blocks fall below `GC_THRESHOLD`
(100 blocks).

### Stage 5 — Multi-block Files
Each inode has 10 direct block pointers (`direct[0]` through `direct[9]`),
supporting files up to **40 KB**. Read and write work correctly across block boundaries.

### Stage 6 — File Deletion (`unlink`)
`rm file` calls `lfs_unlink` which:
1. Zeros the directory entry (slot marked free, `inode_no = 0`)
2. Sets `inode_map[ino] = 0` — inode and all data blocks become dead for GC
3. Checkpoints to disk

Works at any directory depth.

### Stage 7 — Subdirectories (`mkdir`, `rmdir`)
`path_to_inode` walks any depth by splitting the path on `/` and calling
`lookup_in_dir` at each component. All operations (create, read, write, unlink)
automatically work inside subdirectories.

`mkdir` allocates a new directory inode with a fresh empty data block.
`rmdir` refuses to remove non-empty directories (`-ENOTEMPTY`).
Deleted dirent slots are reused by new entries.

### Stage 8 — Crash Recovery
If power cuts out mid-write, the filesystem recovers cleanly on remount.

Every `log_checkpoint` writes in this strict order:
1. Inode map → block 1
2. Superblock (with incremented `commit_seq`) → block 0
3. **Commit block** (magic + seq + log_tail + imap XOR-CRC) → block 2 ← written last

On mount, `log_recover` reads the commit block and checks all four fields against
the superblock. If all match → clean mount. If any mismatch → incomplete checkpoint:
1. Scan backwards from `log_tail` to find the last non-zero block → rewind `log_tail`
2. Scan all log blocks for valid inodes → rebuild inode map from scratch
3. Write a fresh checkpoint to seal the recovered state

### Stage 9 — Indirect Blocks
Adds a single `indirect` pointer to each inode. The indirect block holds 1024
`uint32_t` block pointers, extending the maximum file size from **40 KB to ~4 MB**.

- `block_idx < 10` → resolved via `direct[]` (unchanged)
- `block_idx >= 10` → resolved via `indirect[block_idx - 10]`

The indirect block is copy-on-write: any write to the indirect region reads the
existing indirect block, updates the relevant pointer, and appends a new copy to
the log. GC marks the indirect block and all blocks it points to as live.

---
