#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
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

/*
 * Use pread/pwrite instead of lseek+read/write.
 * pread/pwrite are atomic with respect to the file offset — no risk
 * of a seek/read race if we ever add threads later, and no silent
 * short-reads caused by a prior lseek leaving the cursor in the
 * wrong place.
 */
int disk_read(uint32_t block, void *buf)
{
    if (disk_fd < 0) {
        fprintf(stderr, "disk_read: disk not open\n");
        return -1;
    }

    off_t offset = (off_t)block * BLOCK_SIZE;
    ssize_t n = pread(disk_fd, buf, BLOCK_SIZE, offset);

    if (n < 0) {
        perror("disk_read: pread");
        return -1;
    }
    if (n != BLOCK_SIZE) {
        fprintf(stderr, "disk_read: short read on block %u "
                        "(got %zd bytes)\n", block, n);
        return -1;
    }
    return 0;
}

int disk_write(uint32_t block, const void *buf)
{
    if (disk_fd < 0) {
        fprintf(stderr, "disk_write: disk not open\n");
        return -1;
    }

    off_t offset = (off_t)block * BLOCK_SIZE;
    ssize_t n = pwrite(disk_fd, buf, BLOCK_SIZE, offset);

    if (n < 0) {
        perror("disk_write: pwrite");
        return -1;
    }
    if (n != BLOCK_SIZE) {
        fprintf(stderr, "disk_write: short write on block %u "
                        "(wrote %zd bytes)\n", block, n);
        return -1;
    }
    return 0;
}

void disk_close(void)
{
    if (disk_fd >= 0) {
        close(disk_fd);
        disk_fd = -1;
    }
}
