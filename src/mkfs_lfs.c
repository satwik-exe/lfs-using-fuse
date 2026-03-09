/*
 * mkfs_lfs.c — Format a blank file as an LFS disk image
 *
 * Layout after mkfs:
 *   Block 0  : Superblock
 *   Block 1  : Inode map  (128 × uint32_t)
 *   Block 2  : Commit block  (Stage 8 crash recovery seal)
 *   Block 3  : Root inode (inode 0)
 *   Block 4  : Root directory data
 *   Block 5  : hello.txt data
 *   Block 6  : hello.txt inode (inode 1)
 *   Block 7+ : Free log space  ← log_tail starts here
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "lfs.h"

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

/* XOR checksum over inode map — must match imap_crc() in log.c */
static uint32_t imap_crc(const uint32_t *imap)
{
    uint32_t crc = 0;
    for (int i = 0; i < INODE_MAP_SIZE; i++)
        crc ^= imap[i];
    return crc;
}

int main(void)
{
    int fd = open("../lfs.img", O_CREAT | O_RDWR | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, (off_t)BLOCK_SIZE * TOTAL_BLOCKS) != 0) {
        perror("ftruncate"); return 1;
    }

    uint32_t log_tail = 7;   /* first free block after fixed layout */

    /* ---- Inode map (block 1) ---- */
    uint32_t imap[INODE_MAP_SIZE];
    memset(imap, 0, sizeof(imap));
    imap[0] = 3;   /* root inode at block 3 */
    imap[1] = 6;   /* hello.txt inode at block 6 */
    write_block(fd, 1, imap);

    /* ---- Superblock (block 0) ---- */
    struct lfs_superblock sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic           = LFS_MAGIC;
    sb.block_size      = BLOCK_SIZE;
    sb.total_blocks    = TOTAL_BLOCKS;
    sb.inode_map_block = INODE_MAP_BLOCK;
    sb.log_start       = LOG_START_BLOCK;
    sb.log_tail        = log_tail;
    sb.commit_seq      = 1;            /* first valid sequence number */
    write_block(fd, 0, &sb);

    /* ---- Commit block (block 2) ---- */
    struct lfs_commit commit;
    memset(&commit, 0, sizeof(commit));
    commit.commit_magic = LFS_COMMIT_MAGIC;
    commit.commit_seq   = 1;           /* matches superblock          */
    commit.log_tail     = log_tail;
    commit.imap_crc     = imap_crc(imap);
    write_block(fd, COMMIT_BLOCK, &commit);

    /* ---- Root directory data (block 4) ---- */
    struct lfs_dirent dir_entries[3];
    memset(dir_entries, 0, sizeof(dir_entries));
    dir_entries[0].inode_no = 0; strcpy(dir_entries[0].name, ".");
    dir_entries[1].inode_no = 0; strcpy(dir_entries[1].name, "..");
    dir_entries[2].inode_no = 1; strcpy(dir_entries[2].name, "hello.txt");
    write_block(fd, 4, dir_entries);

    /* ---- Root inode (block 3) ---- */
    struct lfs_inode root;
    memset(&root, 0, sizeof(root));
    root.inode_no  = 0;
    root.type      = INODE_TYPE_DIR;
    root.nlinks    = 2;
    root.size      = 3 * sizeof(struct lfs_dirent);
    root.direct[0] = 4;   /* root dir data at block 4 */
    write_block(fd, 3, &root);

    /* ---- hello.txt data (block 5) ---- */
    const char *msg = "Hello from LFS!\n";
    char data[BLOCK_SIZE];
    memset(data, 0, BLOCK_SIZE);
    strcpy(data, msg);
    write_block(fd, 5, data);

    /* ---- hello.txt inode (block 6) ---- */
    struct lfs_inode hello;
    memset(&hello, 0, sizeof(hello));
    hello.inode_no  = 1;
    hello.type      = INODE_TYPE_FILE;
    hello.size      = (uint32_t)strlen(msg);
    hello.nlinks    = 1;
    hello.direct[0] = 5;
    write_block(fd, 6, &hello);

    close(fd);
    printf("mkfs_lfs: created lfs.img (%d blocks, %d bytes)\n",
           TOTAL_BLOCKS, BLOCK_SIZE * TOTAL_BLOCKS);
    printf("  commit block written (seq=1, tail=%u)\n", log_tail);
    printf("  log tail starts at block %u\n", log_tail);
    return 0;
}
