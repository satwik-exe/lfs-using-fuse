/*
 * mkfs_lfs.c — Format a blank file as an LFS disk image
 *
 * Layout after mkfs:
 *   Block 0  : Superblock
 *   Block 1  : Inode map  (128 × uint32_t)
 *   Block 2  : Root inode (inode 0)
 *   Block 3  : Root directory data
 *   Block 4  : hello.txt data
 *   Block 5  : hello.txt inode (inode 1)
 *   Block 6+ : Free log space  ← log_tail starts here
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "lfs.h"

/* Write exactly one BLOCK_SIZE chunk at the given block offset */
static void write_block(int fd, uint32_t block, const void *data)
{
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    if (data) memcpy(buf, data, BLOCK_SIZE);
    if (pwrite(fd, buf, BLOCK_SIZE, (off_t)block * BLOCK_SIZE)
            != BLOCK_SIZE) {
        perror("write_block");
        exit(1);
    }
}

int main(void)
{
    int fd = open("../lfs.img", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, (off_t)BLOCK_SIZE * TOTAL_BLOCKS) != 0) {
        perror("ftruncate"); return 1;
    }

    uint32_t log_tail = 6;   /* first free block after our fixed layout */

    /* ---- Superblock (block 0) ---- */
    struct lfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic           = LFS_MAGIC;
    sb.block_size      = BLOCK_SIZE;
    sb.total_blocks    = TOTAL_BLOCKS;
    sb.inode_map_block = INODE_MAP_BLOCK;
    sb.log_start       = LOG_START_BLOCK;
    sb.log_tail        = log_tail;          /* where new writes go     */
    write_block(fd, 0, &sb);

    /* ---- Root inode (block 2) ---- */
    struct lfs_inode root;
    memset(&root, 0, sizeof(root));
    root.inode_no  = 0;
    root.type      = INODE_TYPE_DIR;
    root.nlinks    = 2;
    root.direct[0] = 3;   /* root dir data lives in block 3          */
    /* size set after we know how many dirents we write               */

    /* ---- Root directory data (block 3) ---- */
    struct lfs_dirent dir_entries[3];
    memset(dir_entries, 0, sizeof(dir_entries));

    dir_entries[0].inode_no = 0; strcpy(dir_entries[0].name, ".");
    dir_entries[1].inode_no = 0; strcpy(dir_entries[1].name, "..");
    dir_entries[2].inode_no = 1; strcpy(dir_entries[2].name, "hello.txt");

    root.size = 3 * sizeof(struct lfs_dirent);
    write_block(fd, 3, dir_entries);

    /* Finish and write root inode */
    write_block(fd, 2, &root);

    /* ---- hello.txt data (block 4) ---- */
    const char *msg = "Hello from LFS!\n";
    char data[BLOCK_SIZE];
    memset(data, 0, BLOCK_SIZE);
    strcpy(data, msg);
    write_block(fd, 4, data);

    /* ---- hello.txt inode (block 5) ---- */
    struct lfs_inode hello;
    memset(&hello, 0, sizeof(hello));
    hello.inode_no  = 1;
    hello.type      = INODE_TYPE_FILE;
    hello.size      = (uint32_t)strlen(msg);
    hello.nlinks    = 1;
    hello.direct[0] = 4;
    write_block(fd, 5, &hello);

    /* ---- Inode map (block 1) ---- */
    uint32_t imap[INODE_MAP_SIZE];
    memset(imap, 0, sizeof(imap));
    imap[0] = 2;   /* root inode at block 2 */
    imap[1] = 5;   /* hello.txt inode at block 5 */
    write_block(fd, 1, imap);

    close(fd);
    printf("mkfs_lfs: created lfs.img (%d blocks, %d bytes)\n",
           TOTAL_BLOCKS, BLOCK_SIZE * TOTAL_BLOCKS);
    printf("  log tail starts at block %u\n", log_tail);
    return 0;
}
