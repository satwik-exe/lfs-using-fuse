#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include "lfs.h"
static struct lfs_inode hello_inode;
static struct lfs_superblock sb;
static struct lfs_inode root_inode;
static uint32_t inode_map[128];

static void *lfs_init(struct fuse_conn_info *conn,
                      struct fuse_config *cfg)
{
    (void) conn;
    (void) cfg;

    disk_open("lfs.img");
    disk_read(0, &sb);
    disk_read(1, inode_map);
    disk_read(inode_map[0], &root_inode);
    disk_read(inode_map[1], &hello_inode);
    return NULL;
}

static int lfs_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi)
{
    (void) fi;
    memset(st, 0, sizeof(*st));

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_size = root_inode.size;
        return 0;
    }

    if (strcmp(path, "/hello.txt") == 0) {
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        st->st_size = hello_inode.size;   // ✅ IMPORTANT
        return 0;
    }

    return -ENOENT;
}

static int lfs_readdir(const char *path, void *buf,
                       fuse_fill_dir_t filler,
                       off_t off, struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void) off; (void) fi; (void) flags;

    if (strcmp(path, "/") != 0)
        return -ENOENT;

    struct lfs_dirent d[16] = {0};
    disk_read(root_inode.direct[0], d);

    int n = root_inode.size / sizeof(struct lfs_dirent);
    for (int i = 0; i < n; i++)
        filler(buf, d[i].name, NULL, 0, 0);

    return 0;
}

static int lfs_read(const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void) fi;

    if (strcmp(path, "/hello.txt") != 0)
        return -ENOENT;

    if (offset >= hello_inode.size)
        return 0;

    char data[BLOCK_SIZE];
    disk_read(hello_inode.direct[0], data);

    if (offset + size > hello_inode.size)
        size = hello_inode.size - offset;

    memcpy(buf, data + offset, size);
    return size;
}

static struct fuse_operations ops = {
    .init = lfs_init,
    .getattr = lfs_getattr,
    .readdir = lfs_readdir,
    .read = lfs_read
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &ops, NULL);
}
