/*
 * gc.c — Garbage Collector for LFS
 *
 * Compaction strategy:
 *   1. Find all live blocks
 *   2. Find the earliest free block (hole) in the log
 *   3. Move live blocks from the END of the log into those holes
 *   4. Rewind log_tail past the now-empty tail region
 *
 * This "compact from the end" approach actually shrinks the log.
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

int log_append_ex(struct lfs_state *state, const void *buf,
                  uint32_t inode_no, uint32_t block_idx);

int gc_should_run(struct lfs_state *state)
{
    if (!state) return 0;
    uint32_t free_blocks = state->sb.total_blocks - state->log_tail;
    return (free_blocks < GC_THRESHOLD) ? 1 : 0;
}

static int is_block_live(struct lfs_state *state, uint32_t block)
{
    if (block < LOG_START_BLOCK) return 1;

    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0) continue;
        if (state->inode_map[i] == block) return 1;

        uint8_t buf[BLOCK_SIZE];
        if (disk_read(state->inode_map[i], buf) != 0) continue;
        struct lfs_inode *inode = (struct lfs_inode *)buf;
        for (int j = 0; j < MAX_DIRECT_PTRS; j++) {
            if (inode->direct[j] == block) return 1;
        }
    }
    return 0;
}

/*
 * update_references: after moving a block from old_block to new_block,
 * patch the inode map and inode direct pointers to reflect the new location.
 */
static void update_references(struct lfs_state *state,
                               uint32_t old_block, uint32_t new_block)
{
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == old_block) {
            state->inode_map[i] = new_block;
            printf("GC: inode %d block %u -> %u\n", i, old_block, new_block);
            return;
        }
    }
    /* It's a data block — find which inode owns it */
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(state->inode_map[i], buf) != 0) continue;
        struct lfs_inode *inode = (struct lfs_inode *)buf;
        for (int j = 0; j < MAX_DIRECT_PTRS; j++) {
            if (inode->direct[j] == old_block) {
                inode->direct[j] = new_block;
                disk_write(state->inode_map[i], buf);
                printf("GC: data block inode=%d[%d] %u -> %u\n",
                       i, j, old_block, new_block);
                return;
            }
        }
    }
}

int gc_collect(struct lfs_state *state)
{
    if (!state) return -1;

    uint32_t old_tail = state->log_tail;
    printf("GC: starting, log_tail=%u free=%u\n",
           old_tail, state->sb.total_blocks - old_tail);

    /* Count total dead blocks */
    int total_dead = 0;
    for (uint32_t b = LOG_START_BLOCK; b < state->log_tail; b++)
        if (!is_block_live(state, b)) total_dead++;

    printf("GC: dead=%d used=%u\n", total_dead,
           state->log_tail - LOG_START_BLOCK);

    if (total_dead == 0) {
        printf("GC: nothing to collect\n");
        return 0;
    }

    /*
     * Compact: walk a "dst" pointer forward through holes,
     * and a "src" pointer backward through live blocks.
     * Copy src -> dst when dst is dead and src is live.
     */
    uint32_t dst = LOG_START_BLOCK;
    uint32_t src = state->log_tail - 1;
    int moved = 0;

    while (dst < src) {
        /* Advance dst to next dead block */
        while (dst < src && is_block_live(state, dst))
            dst++;

        /* Retreat src to previous live block */
        while (src > dst && !is_block_live(state, src))
            src--;

        if (dst >= src) break;

        /* Move live block at src into dead slot at dst */
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(src, buf) != 0) { src--; continue; }
        disk_write(dst, buf);

        /* Zero the old location */
        uint8_t zero[BLOCK_SIZE];
        memset(zero, 0, BLOCK_SIZE);
        disk_write(src, zero);

        /* Update all pointers from src -> dst */
        update_references(state, src, dst);

        moved++;
        dst++;
        src--;
    }

    printf("GC: moved %d blocks\n", moved);

    /* Rewind log_tail: find highest live block */
    uint32_t new_tail = LOG_START_BLOCK;
    for (uint32_t b = LOG_START_BLOCK; b < old_tail; b++)
        if (is_block_live(state, b)) new_tail = b + 1;

    /* Round up to segment boundary */
    if (new_tail % BLOCKS_PER_SEGMENT != 0)
        new_tail = ((new_tail / BLOCKS_PER_SEGMENT) + 1) * BLOCKS_PER_SEGMENT;

    printf("GC: rewound log_tail %u -> %u (reclaimed %u blocks)\n",
           old_tail, new_tail, old_tail - new_tail);

    state->log_tail    = new_tail;
    state->sb.log_tail = new_tail;

    log_checkpoint(state);
    printf("GC: done, log_tail=%u free=%u\n",
           state->log_tail,
           state->sb.total_blocks - state->log_tail);
    return 0;
}
