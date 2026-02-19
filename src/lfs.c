/*
 * lfs.c — FUSE frontend for the Log-Structured Filesystem
 *
 * All paths go through the inode map — nothing is hardcoded.
 * Supported operations:
 *   getattr, readdir, read          (read path)
 *   create, write, truncate         (write path — Phase 2)
 *   mkdir                           (directory creation)
 */

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lfs.h"

/* Single global state object */
static struct lfs_state g_state;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/*
 * path_to_inode
 *
 * Resolves an absolute path to an inode number.
 * Currently supports one level deep: "/" and "/filename".
 * Returns the inode number (>=0) or -ENOENT.
 */
static int path_to_inode(const char *path)
{
    if (strcmp(path, "/") == 0)
        return 0;   /* root is always inode 0 */

    /* Strip the leading slash */
    const char *name = path + 1;

    /* No subdirectory support yet — reject embedded slashes */
    if (strchr(name, '/') != NULL)
        return -ENOENT;

    /* Read root directory data block and search for the name */
    struct lfs_inode root;
    if (inode_read(&g_state, 0, &root) != 0)
        return -EIO;

    uint8_t buf[BLOCK_SIZE];
    if (disk_read(root.direct[0], buf) != 0)
        return -EIO;

    int n = root.size / sizeof(struct lfs_dirent);
    struct lfs_dirent *entries = (struct lfs_dirent *)buf;

    for (int i = 0; i < n; i++) {
        if (entries[i].inode_no != 0 &&
            strcmp(entries[i].name, name) == 0)
            return (int)entries[i].inode_no;
    }
    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  FUSE operations                                                     */
/* ------------------------------------------------------------------ */

static void *lfs_init(struct fuse_conn_info *conn,
                      struct fuse_config   *cfg)
{
    (void)conn; (void)cfg;

    memset(&g_state, 0, sizeof(g_state));

    if (disk_open("../lfs.img") != 0) {
        fprintf(stderr, "lfs_init: cannot open lfs.img\n");
        return NULL;
    }

    /* Read superblock */
    uint8_t buf[BLOCK_SIZE];
    if (disk_read(0, buf) != 0) {
        fprintf(stderr, "lfs_init: cannot read superblock\n");
        return NULL;
    }
    memcpy(&g_state.sb, buf, sizeof(g_state.sb));

    if (g_state.sb.magic != LFS_MAGIC) {
        fprintf(stderr, "lfs_init: bad magic 0x%x (expected 0x%x)\n",
                g_state.sb.magic, LFS_MAGIC);
        return NULL;
    }

    /* Read inode map */
    if (disk_read(INODE_MAP_BLOCK, buf) != 0) {
        fprintf(stderr, "lfs_init: cannot read inode map\n");
        return NULL;
    }
    memcpy(g_state.inode_map, buf, INODE_MAP_SIZE * sizeof(uint32_t));

    /* Restore log tail from superblock */
    g_state.log_tail = g_state.sb.log_tail;

    printf("LFS mounted: %u blocks, log tail at block %u\n",
           g_state.sb.total_blocks, g_state.log_tail);
    return &g_state;
}

static void lfs_destroy(void *private_data)
{
    (void)private_data;
    log_checkpoint(&g_state);
    disk_close();
    printf("LFS unmounted.\n");
}

static int lfs_getattr(const char *path, struct stat *st,
                       struct fuse_file_info *fi)
{
    (void)fi;
    memset(st, 0, sizeof(*st));

    int ino = path_to_inode(path);
    if (ino < 0) return ino;   /* -ENOENT or -EIO */

    struct lfs_inode inode;
    if (inode_read(&g_state, (uint32_t)ino, &inode) != 0)
        return -EIO;

    st->st_ino   = inode.inode_no;
    st->st_nlink = inode.nlinks ? inode.nlinks : 1;
    st->st_size  = inode.size;

    if (inode.type == INODE_TYPE_DIR) {
        st->st_mode  = S_IFDIR | 0755;
        st->st_nlink = 2;
    } else {
        st->st_mode  = S_IFREG | 0644;
    }
    return 0;
}

static int lfs_readdir(const char *path, void *buf,
                       fuse_fill_dir_t filler, off_t off,
                       struct fuse_file_info *fi,
                       enum fuse_readdir_flags flags)
{
    (void)off; (void)fi; (void)flags;

    int ino = path_to_inode(path);
    if (ino < 0) return ino;

    struct lfs_inode dir_inode;
    if (inode_read(&g_state, (uint32_t)ino, &dir_inode) != 0)
        return -EIO;
    if (dir_inode.type != INODE_TYPE_DIR)
        return -ENOTDIR;

    filler(buf, ".",  NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    uint8_t dbuf[BLOCK_SIZE];
    if (disk_read(dir_inode.direct[0], dbuf) != 0)
        return -EIO;

    int n = dir_inode.size / sizeof(struct lfs_dirent);
    struct lfs_dirent *entries = (struct lfs_dirent *)dbuf;

    for (int i = 0; i < n; i++) {
        if (entries[i].inode_no != 0 &&
            strcmp(entries[i].name, ".") != 0 &&
            strcmp(entries[i].name, "..") != 0) {
            filler(buf, entries[i].name, NULL, 0, 0);
        }
    }
    return 0;
}

static int lfs_read(const char *path, char *buf, size_t size,
                    off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    int ino = path_to_inode(path);
    if (ino < 0) return ino;

    struct lfs_inode inode;
    if (inode_read(&g_state, (uint32_t)ino, &inode) != 0)
        return -EIO;
    if (inode.type != INODE_TYPE_FILE)
        return -EISDIR;

    if (offset >= (off_t)inode.size) return 0;

    if (offset + (off_t)size > (off_t)inode.size)
        size = inode.size - offset;

    /* Simple single-block read for now (files <= 4 KB) */
    uint8_t data[BLOCK_SIZE];
    if (disk_read(inode.direct[0], data) != 0)
        return -EIO;

    memcpy(buf, data + offset, size);
    return (int)size;
}

/*
 * lfs_create
 *
 * Creates a new empty regular file and adds it to the root dir.
 * This is the entry point for the LFS write path:
 *   1. Allocate an inode number
 *   2. Append an empty data block to the log
 *   3. Append the new inode to the log
 *   4. Add a directory entry in the root dir
 *   5. Checkpoint
 */
static int lfs_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)mode; (void)fi;

    /* Only support files directly under root */
    if (path[0] != '/' || strchr(path + 1, '/') != NULL)
        return -EPERM;

    const char *name = path + 1;
    if (strlen(name) >= MAX_NAME_LEN)
        return -ENAMETOOLONG;

    /* Reject if it already exists */
    if (path_to_inode(path) != -ENOENT)
        return -EEXIST;

    /* 1. Allocate inode number */
    int ino = inode_alloc(&g_state);
    if (ino < 0) return -ENOSPC;

    /* 2. Append an empty data block (the file's first data block) */
    uint8_t zero_block[BLOCK_SIZE];
    memset(zero_block, 0, BLOCK_SIZE);
    int data_block = log_append(&g_state, zero_block);
    if (data_block < 0) return -ENOSPC;

    /* 3. Build and append the inode */
    struct lfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_no  = (uint32_t)ino;
    new_inode.type      = INODE_TYPE_FILE;
    new_inode.size      = 0;
    new_inode.nlinks    = 1;
    new_inode.direct[0] = (uint32_t)data_block;

    if (inode_write(&g_state, &new_inode) != 0) return -EIO;

    /* 4. Add directory entry to root */
    struct lfs_inode root;
    if (inode_read(&g_state, 0, &root) != 0) return -EIO;

    uint8_t dbuf[BLOCK_SIZE];
    if (disk_read(root.direct[0], dbuf) != 0) return -EIO;

    int slot = root.size / sizeof(struct lfs_dirent);
    if (slot * sizeof(struct lfs_dirent) >= BLOCK_SIZE)
        return -ENOSPC;   /* directory full */

    struct lfs_dirent *entries = (struct lfs_dirent *)dbuf;
    entries[slot].inode_no = (uint32_t)ino;
    strncpy(entries[slot].name, name, MAX_NAME_LEN - 1);

    /* Append the updated directory data block */
    int new_dir_block = log_append(&g_state, dbuf);
    if (new_dir_block < 0) return -ENOSPC;

    /* Update root inode to point to new dir block */
    root.direct[0] = (uint32_t)new_dir_block;
    root.size      += sizeof(struct lfs_dirent);
    if (inode_write(&g_state, &root) != 0) return -EIO;

    /* 5. Checkpoint */
    return log_checkpoint(&g_state);
}

/*
 * lfs_write
 *
 * Writes data into an existing file.
 * True LFS behaviour:
 *   - Copy existing data + new data into a buffer
 *   - Append the new data block to the log
 *   - Append a new version of the inode to the log
 *   - Checkpoint
 *
 * Currently limited to single-block (4 KB) files.
 */
static int lfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    int ino = path_to_inode(path);
    if (ino < 0) return ino;

    struct lfs_inode inode;
    if (inode_read(&g_state, (uint32_t)ino, &inode) != 0)
        return -EIO;
    if (inode.type != INODE_TYPE_FILE)
        return -EISDIR;

    /* Enforce single-block limit for now */
    if (offset + (off_t)size > BLOCK_SIZE) {
        fprintf(stderr, "lfs_write: write exceeds single block limit\n");
        return -EFBIG;
    }

    /* Read current data block */
    uint8_t data[BLOCK_SIZE];
    memset(data, 0, BLOCK_SIZE);
    if (inode.direct[0] != 0)
        disk_read(inode.direct[0], data);

    /* Overlay new data */
    memcpy(data + offset, buf, size);

    /* Update file size */
    uint32_t new_end = (uint32_t)(offset + size);
    if (new_end > inode.size)
        inode.size = new_end;

    /* Append new data block to log */
    int new_data_block = log_append(&g_state, data);
    if (new_data_block < 0) return -ENOSPC;

    /* Update inode to point to new data block, append to log */
    inode.direct[0] = (uint32_t)new_data_block;
    if (inode_write(&g_state, &inode) != 0) return -EIO;

    /* Checkpoint */
    if (log_checkpoint(&g_state) != 0) return -EIO;

    return (int)size;
}

/*
 * lfs_truncate — required by many editors before writing
 */
static int lfs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;
    if (size != 0) return -EPERM;   /* only support truncate-to-zero */

    int ino = path_to_inode(path);
    if (ino < 0) return ino;

    struct lfs_inode inode;
    if (inode_read(&g_state, (uint32_t)ino, &inode) != 0)
        return -EIO;

    inode.size = 0;
    if (inode_write(&g_state, &inode) != 0) return -EIO;
    return log_checkpoint(&g_state);
}

/* ------------------------------------------------------------------ */
/*  FUSE ops table + main                                               */
/* ------------------------------------------------------------------ */

static struct fuse_operations lfs_ops = {
    .init     = lfs_init,
    .destroy  = lfs_destroy,
    .getattr  = lfs_getattr,
    .readdir  = lfs_readdir,
    .read     = lfs_read,
    .create   = lfs_create,
    .write    = lfs_write,
    .truncate = lfs_truncate,
};

int main(int argc, char *argv[])
{
    return fuse_main(argc, argv, &lfs_ops, NULL);
}
