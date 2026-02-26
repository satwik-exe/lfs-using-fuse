/*
 * lfs.c — FUSE frontend for the Log-Structured Filesystem
 *
 * All paths go through the inode map — nothing is hardcoded.
 * Supported operations:
 *   getattr, readdir, read          (read path)
 *   create, write, truncate         (write path)
 */

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lfs.h"

/* Extended log append declared in log.c */
int log_append_ex(struct lfs_state *state, const void *buf,
                  uint32_t inode_no, uint32_t block_idx);

/* Single global state object */
static struct lfs_state g_state;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static int path_to_inode(const char *path)
{
    if (strcmp(path, "/") == 0)
        return 0;

    const char *name = path + 1;

    if (strchr(name, '/') != NULL)
        return -ENOENT;

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
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    (void)conn; (void)cfg;

    memset(&g_state, 0, sizeof(g_state));

    if (disk_open("/home/kiit/lfs-fuse/lfs.img") != 0) {
        fprintf(stderr, "lfs_init: cannot open lfs.img\n");
        return NULL;
    }

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

    if (disk_read(INODE_MAP_BLOCK, buf) != 0) {
        fprintf(stderr, "lfs_init: cannot read inode map\n");
        return NULL;
    }
    memcpy(g_state.inode_map, buf, INODE_MAP_SIZE * sizeof(uint32_t));

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
    if (ino < 0) return ino;

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

    uint8_t data[BLOCK_SIZE];
    if (disk_read(inode.direct[0], data) != 0)
        return -EIO;

    memcpy(buf, data + offset, size);
    return (int)size;
}

static int lfs_create(const char *path, mode_t mode,
                      struct fuse_file_info *fi)
{
    (void)mode; (void)fi;

    printf("lfs_create: path=%s log_tail=%u free=%u\n",
           path, g_state.log_tail,
           g_state.sb.total_blocks - g_state.log_tail);

    if (path[0] != '/' || strchr(path + 1, '/') != NULL)
        return -EPERM;

    const char *name = path + 1;
    if (strlen(name) >= MAX_NAME_LEN)
        return -ENAMETOOLONG;

    if (path_to_inode(path) != -ENOENT)
        return -EEXIST;

    int ino = inode_alloc(&g_state);
    if (ino < 0) return -ENOSPC;

    if (gc_should_run(&g_state)) {
        printf("lfs_create: GC triggered! free=%u\n",
               g_state.sb.total_blocks - g_state.log_tail);
        gc_collect(&g_state);
    }

    uint8_t zero_block[BLOCK_SIZE];
    memset(zero_block, 0, BLOCK_SIZE);
    int data_block = log_append_ex(&g_state, zero_block,
                                   (uint32_t)ino, 0);
    if (data_block < 0) return -ENOSPC;

    struct lfs_inode new_inode;
    memset(&new_inode, 0, sizeof(new_inode));
    new_inode.inode_no  = (uint32_t)ino;
    new_inode.type      = INODE_TYPE_FILE;
    new_inode.size      = 0;
    new_inode.nlinks    = 1;
    new_inode.direct[0] = (uint32_t)data_block;

    if (inode_write(&g_state, &new_inode) != 0) return -EIO;

    struct lfs_inode root;
    if (inode_read(&g_state, 0, &root) != 0) return -EIO;

    uint8_t dbuf[BLOCK_SIZE];
    if (disk_read(root.direct[0], dbuf) != 0) return -EIO;

    int slot = root.size / sizeof(struct lfs_dirent);
    if (slot * (int)sizeof(struct lfs_dirent) >= BLOCK_SIZE)
        return -ENOSPC;

    struct lfs_dirent *entries = (struct lfs_dirent *)dbuf;
    entries[slot].inode_no = (uint32_t)ino;
    strncpy(entries[slot].name, name, MAX_NAME_LEN - 1);

    int new_dir_block = log_append_ex(&g_state, dbuf, 0, 0);
    if (new_dir_block < 0) return -ENOSPC;

    root.direct[0] = (uint32_t)new_dir_block;
    root.size      += sizeof(struct lfs_dirent);
    if (inode_write(&g_state, &root) != 0) return -EIO;

    printf("lfs_create: done, new log_tail=%u\n", g_state.log_tail);
    return log_checkpoint(&g_state);
}

static int lfs_write(const char *path, const char *buf, size_t size,
                     off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    printf("lfs_write: path=%s size=%zu offset=%ld log_tail=%u free=%u\n",
           path, size, offset, g_state.log_tail,
           g_state.sb.total_blocks - g_state.log_tail);

    int ino = path_to_inode(path);
    if (ino < 0) return ino;

    struct lfs_inode inode;
    if (inode_read(&g_state, (uint32_t)ino, &inode) != 0)
        return -EIO;
    if (inode.type != INODE_TYPE_FILE)
        return -EISDIR;

    if (offset + (off_t)size > BLOCK_SIZE) {
        fprintf(stderr, "lfs_write: exceeds single block limit\n");
        return -EFBIG;
    }

    uint8_t data[BLOCK_SIZE];
    memset(data, 0, BLOCK_SIZE);
    if (inode.direct[0] != 0)
        disk_read(inode.direct[0], data);

    memcpy(data + offset, buf, size);

    uint32_t new_end = (uint32_t)(offset + size);
    if (new_end > inode.size)
        inode.size = new_end;

    if (gc_should_run(&g_state)) {
        printf("lfs_write: GC triggered! free=%u\n",
               g_state.sb.total_blocks - g_state.log_tail);
        gc_collect(&g_state);
    }

    int new_data_block = log_append_ex(&g_state, data,
                                       (uint32_t)ino, 0);
    if (new_data_block < 0) return -ENOSPC;

    inode.direct[0] = (uint32_t)new_data_block;
    if (inode_write(&g_state, &inode) != 0) return -EIO;

    if (log_checkpoint(&g_state) != 0) return -EIO;

    printf("lfs_write: done, new log_tail=%u\n", g_state.log_tail);
    return (int)size;
}

static int lfs_truncate(const char *path, off_t size,
                        struct fuse_file_info *fi)
{
    (void)fi;

    printf("lfs_truncate: path=%s size=%ld\n", path, size);

    if (size != 0) return -EPERM;

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
