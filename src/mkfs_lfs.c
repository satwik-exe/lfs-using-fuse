#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "lfs.h"

#define TOTAL_BLOCKS 100

int main()
{
    int fd = open("lfs.img", O_CREAT | O_RDWR | O_TRUNC, 0666);
    ftruncate(fd, BLOCK_SIZE * TOTAL_BLOCKS);

    /* -------- Superblock (block 0) -------- */
    struct lfs_superblock sb = {
        .magic = LFS_MAGIC,
        .block_size = BLOCK_SIZE,
        .total_blocks = TOTAL_BLOCKS,
        .inode_map_start = 1,
        .log_start = 10
    };
    write(fd, &sb, sizeof(sb));

    /* -------- Inode map (block 1) -------- */
    uint32_t imap[128] = {0};

    /* -------- Root inode (block 2) -------- */
    struct lfs_inode root = {0};
    root.inode_no = 0;
    root.type = INODE_TYPE_DIR;

    imap[0] = 2;
    lseek(fd, 2 * BLOCK_SIZE, SEEK_SET);
    write(fd, &root, sizeof(root));

    /* -------- Root directory block (block 3) -------- */
    struct lfs_dirent dir[3] = {0};

    dir[0].inode_no = 0; strcpy(dir[0].name, ".");
    dir[1].inode_no = 0; strcpy(dir[1].name, "..");

    lseek(fd, 3 * BLOCK_SIZE, SEEK_SET);
    write(fd, dir, sizeof(dir));

    root.direct[0] = 3;
    root.size = 2 * sizeof(struct lfs_dirent);
    lseek(fd, 2 * BLOCK_SIZE, SEEK_SET);
    write(fd, &root, sizeof(root));

    /* -------- hello.txt data (block 4) -------- */
    /* -------- hello.txt data (block 4) -------- */
const char *msg = "Hello from LFS\n";

char data[BLOCK_SIZE];
memset(data, 0, BLOCK_SIZE);
strcpy(data, msg);

lseek(fd, 4 * BLOCK_SIZE, SEEK_SET);
write(fd, data, BLOCK_SIZE);

/* -------- hello.txt inode (block 5) -------- */
struct lfs_inode hello;
memset(&hello, 0, sizeof(hello));

hello.inode_no = 1;
hello.type = INODE_TYPE_FILE;
hello.size = strlen(msg);    // ✅ CORRECT
hello.direct[0] = 4;

imap[1] = 5;
lseek(fd, 5 * BLOCK_SIZE, SEEK_SET);
write(fd, &hello, sizeof(hello));

    /* -------- Add hello.txt to root dir -------- */
    struct lfs_dirent e;
    e.inode_no = 1;
    strcpy(e.name, "hello.txt");

    lseek(fd, 3 * BLOCK_SIZE + 2 * sizeof(struct lfs_dirent), SEEK_SET);
    write(fd, &e, sizeof(e));

    root.size += sizeof(struct lfs_dirent);
    lseek(fd, 2 * BLOCK_SIZE, SEEK_SET);
    write(fd, &root, sizeof(root));

    /* -------- Write inode map -------- */
    lseek(fd, 1 * BLOCK_SIZE, SEEK_SET);
    write(fd, imap, sizeof(imap));

    close(fd);
    return 0;
}
