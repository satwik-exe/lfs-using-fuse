#ifndef LFS_H
#define LFS_H

#include <stdint.h>

/* -------- Constants -------- */

#define LFS_MAGIC 0x4C465331
#define BLOCK_SIZE 4096

#define INODE_TYPE_FILE 1
#define INODE_TYPE_DIR  2

#define MAX_DIRECT_PTRS 10
#define MAX_NAME_LEN 28

/* -------- Superblock -------- */

struct lfs_superblock {
    uint32_t magic;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t inode_map_start;
    uint32_t log_start;
};

/* -------- Inode -------- */

struct lfs_inode {
    uint32_t inode_no;
    uint32_t type;
    uint32_t size;
    uint32_t direct[MAX_DIRECT_PTRS];
};

/* -------- Directory entry -------- */

struct lfs_dirent {
    uint32_t inode_no;
    char name[MAX_NAME_LEN];
};

/* -------- Disk API -------- */

int disk_open(const char *path);
int disk_read(uint32_t block, void *buf);
int disk_write(uint32_t block, const void *buf);
void disk_close(void);

#endif
