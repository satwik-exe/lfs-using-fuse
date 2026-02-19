/*
 * inode.c — Inode management
 *
 * Inodes are NOT stored at fixed locations on disk.  In LFS every
 * inode write appends a new copy to the log and updates the inode
 * map so future reads find the latest version.
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

/*
 * inode_read
 *
 * Looks up inode 'ino' in the inode map to find its current block,
 * then reads that block.
 * Returns 0 on success, -1 on error.
 */
int inode_read(struct lfs_state *state, uint32_t ino,
               struct lfs_inode *out)
{
    if (!state || !out) return -1;

    if (ino >= INODE_MAP_SIZE) {
        fprintf(stderr, "inode_read: ino %u out of range\n", ino);
        return -1;
    }

    uint32_t block = state->inode_map[ino];
    if (block == 0) {
        fprintf(stderr, "inode_read: ino %u not allocated "
                        "(imap[%u]=0)\n", ino, ino);
        return -1;
    }

    uint8_t buf[BLOCK_SIZE];
    if (disk_read(block, buf) != 0) return -1;

    memcpy(out, buf, sizeof(struct lfs_inode));
    return 0;
}

/*
 * inode_write
 *
 * Appends 'in' to the log and updates inode_map[in->inode_no].
 * Returns 0 on success, -1 on error.
 *
 * This is the correct LFS behaviour: every inode update creates a
 * new on-disk copy rather than overwriting the old one.
 */
int inode_write(struct lfs_state *state, const struct lfs_inode *in)
{
    if (!state || !in) return -1;

    if (in->inode_no >= INODE_MAP_SIZE) {
        fprintf(stderr, "inode_write: ino %u out of range\n",
                in->inode_no);
        return -1;
    }

    /* Pack the inode into a full block (rest is zeroed) */
    uint8_t buf[BLOCK_SIZE];
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, in, sizeof(struct lfs_inode));

    int block = log_append(state, buf);
    if (block < 0) return -1;

    /* Update the in-memory inode map */
    state->inode_map[in->inode_no] = (uint32_t)block;
    return 0;
}

/*
 * inode_alloc
 *
 * Scans the inode map for the first entry that is 0 (unused).
 * Inode 0 is always the root directory, so scanning starts at 0
 * but 0 is valid — it just must already be initialised by mkfs.
 * For new allocations we start at 1.
 *
 * Returns the allocated inode number, or -1 if the map is full.
 */
int inode_alloc(struct lfs_state *state)
{
    if (!state) return -1;

    /* Start from 1 — inode 0 is root, allocated by mkfs */
    for (int i = 1; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0)
            return i;
    }
    fprintf(stderr, "inode_alloc: inode map is full\n");
    return -1;
}
