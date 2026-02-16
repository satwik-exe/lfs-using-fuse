#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include "lfs.h"

static int disk_fd = -1;

int disk_open(const char *path)
{
    disk_fd = open(path, O_RDWR);
    if (disk_fd < 0) {
        perror("disk_open");
        return -1;
    }
    return 0;
}

int disk_read(uint32_t block, void *buf)
{
    lseek(disk_fd, block * BLOCK_SIZE, SEEK_SET);
    return read(disk_fd, buf, BLOCK_SIZE);
}

int disk_write(uint32_t block, const void *buf)
{
    lseek(disk_fd, block * BLOCK_SIZE, SEEK_SET);
    return write(disk_fd, buf, BLOCK_SIZE);
}

void disk_close(void)
{
    if (disk_fd >= 0)
        close(disk_fd);
}
