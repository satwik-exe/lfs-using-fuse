#ifndef LFS_H
#define LFS_H

#include <stdint.h>
#include <sys/stat.h>

/* ================================================================
   Constants
   ================================================================ */

#define LFS_MAGIC        0x4C465331
#define BLOCK_SIZE       4096
#define TOTAL_BLOCKS     1024          /* 4 MB disk image            */
#define INODE_MAP_BLOCK  1             /* block where inode map lives */
#define INODE_MAP_SIZE   128           /* max inodes supported        */
#define LOG_START_BLOCK  6             /* first block usable for log  */

/* Segment = 32 blocks = 128 KB                                      */
#define BLOCKS_PER_SEGMENT  32
#define SEGMENT_COUNT       (TOTAL_BLOCKS / BLOCKS_PER_SEGMENT)

/* GC triggers when free blocks drop below this threshold            */
#define GC_THRESHOLD        100

#define INODE_TYPE_FILE  1
#define INODE_TYPE_DIR   2

#define MAX_DIRECT_PTRS  10
#define MAX_NAME_LEN     28

/* ================================================================
   On-disk structures  (all must fit inside BLOCK_SIZE)
   ================================================================ */

/* Block 0 */
struct lfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_map_block;  /* which block holds the inode map   */
    uint32_t log_start;        /* first writable log block          */
    uint32_t log_tail;         /* next free block in the log        */
    uint32_t commit_seq;       /* sequence number of last commit    */
    uint8_t  _pad[BLOCK_SIZE - 7*sizeof(uint32_t)];
} __attribute__((packed));

/* One inode — stored inside a log block */
struct lfs_inode {
    uint32_t inode_no;
    uint32_t type;             /* INODE_TYPE_FILE or INODE_TYPE_DIR */
    uint32_t size;             /* bytes                             */
    uint32_t nlinks;
    uint32_t direct[MAX_DIRECT_PTRS]; /* block numbers for data     */
    uint8_t  _pad[BLOCK_SIZE - (4 + MAX_DIRECT_PTRS)*sizeof(uint32_t)];
} __attribute__((packed));

/* One directory entry */
struct lfs_dirent {
    uint32_t inode_no;         /* 0 = free slot                     */
    char     name[MAX_NAME_LEN];
};

/*
 * Segment summary — stored as the FIRST block of every segment.
 */
struct lfs_segment_summary {
    struct {
        uint32_t inode_no;
        uint32_t block_idx;
    } entry[BLOCKS_PER_SEGMENT];
    uint8_t _pad[BLOCK_SIZE
                 - BLOCKS_PER_SEGMENT * 2 * sizeof(uint32_t)];
} __attribute__((packed));

/*
 * Commit block — Stage 8 crash recovery.
 *
 * Written as the LAST action of every log_checkpoint.
 * Stored at a fixed location: block 2 (just after inode map).
 *
 * Recovery logic on mount:
 *   Read the commit block.  If commit_magic matches and
 *   commit_seq == superblock.commit_seq, the last checkpoint
 *   completed fully — trust the superblock's log_tail.
 *
 *   Otherwise the last checkpoint was interrupted mid-write.
 *   Scan forward from log_start to find the true end of the log
 *   and rewind log_tail to the last fully-written block.
 *
 * The checksum is a simple XOR of all inode_map[] entries so we
 * can detect a partially-written inode map block.
 */
#define LFS_COMMIT_MAGIC  0xC0FFEE42
#define COMMIT_BLOCK      2            /* fixed location on disk     */

struct lfs_commit {
    uint32_t commit_magic;   /* LFS_COMMIT_MAGIC                    */
    uint32_t commit_seq;     /* must match superblock.commit_seq    */
    uint32_t log_tail;       /* log_tail at time of this checkpoint */
    uint32_t imap_crc;       /* XOR checksum of inode_map[]         */
    uint8_t  _pad[BLOCK_SIZE - 4*sizeof(uint32_t)];
} __attribute__((packed));

/* ================================================================
   In-memory runtime state  (not written to disk as a unit)
   ================================================================ */
struct lfs_state {
    int      disk_fd;
    struct   lfs_superblock sb;
    uint32_t inode_map[INODE_MAP_SIZE];
    uint32_t log_tail;         /* mirrors sb.log_tail, updated live  */
};

/* ================================================================
   Disk layer API  (disk.c)
   ================================================================ */
int  disk_open (const char *path);
int  disk_read (uint32_t block, void *buf);
int  disk_write(uint32_t block, const void *buf);
void disk_close(void);

/* ================================================================
   Log layer API  (log.c)
   ================================================================ */
int  log_append    (struct lfs_state *state, const void *buf);
int  log_checkpoint(struct lfs_state *state);
int  log_recover   (struct lfs_state *state);   /* Stage 8 */

/* ================================================================
   Inode helpers  (inode.c)
   ================================================================ */
int  inode_read (struct lfs_state *state, uint32_t ino,
                 struct lfs_inode *out);
int  inode_write(struct lfs_state *state, const struct lfs_inode *in);
int  inode_alloc(struct lfs_state *state);

/* ================================================================
   Garbage collector  (gc.c)
   ================================================================ */
int  gc_should_run(struct lfs_state *state);
int  gc_collect   (struct lfs_state *state);

#endif /* LFS_H */
