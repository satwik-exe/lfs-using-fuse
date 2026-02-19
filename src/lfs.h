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
#define LOG_START_BLOCK  10            /* first block usable for log  */

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
    uint8_t  _pad[BLOCK_SIZE - 6*sizeof(uint32_t)];
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

/* Block 1: array of INODE_MAP_SIZE uint32_t block addresses.
   inode_map[ino] = block number where that inode currently lives.
   0 means the inode does not exist.                                 */

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

/* Append one 4 KB block to the log; returns the block number written
   to, or -1 on error.                                               */
int  log_append(struct lfs_state *state, const void *buf);

/* Persist the inode map and superblock (called after every mutation) */
int  log_checkpoint(struct lfs_state *state);

/* ================================================================
   Inode helpers  (inode.c)
   ================================================================ */

/* Read the inode for 'ino' into *out.  Returns 0 on success.        */
int  inode_read (struct lfs_state *state, uint32_t ino,
                 struct lfs_inode *out);

/* Write (append) an inode to the log and update the inode map.
   Returns 0 on success.                                             */
int  inode_write(struct lfs_state *state, const struct lfs_inode *in);

/* Allocate the next free inode number.  Returns -1 if full.         */
int  inode_alloc(struct lfs_state *state);

#endif /* LFS_H */
