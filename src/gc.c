/*
 * gc.c — Garbage Collector for LFS
 *
 * Strategy:
 *   1. Scan all segments, count dead blocks in each
 *   2. Pick the segment with the most dead blocks
 *   3. Copy its live blocks to the end of the log
 *   4. Update inode map to point to new locations
 *   5. Zero out the old segment so it can be reused
 *   6. Reset log_tail to reclaim the freed space
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

/* Declare the extended log append from log.c */
int log_append_ex(struct lfs_state *state, const void *buf,
                  uint32_t inode_no, uint32_t block_idx);

/*
 * gc_should_run
 *
 * Returns 1 if free blocks < GC_THRESHOLD, 0 otherwise.
 */
int gc_should_run(struct lfs_state *state)
{
    if (!state) return 0;
    uint32_t free_blocks = state->sb.total_blocks - state->log_tail;
    return (free_blocks < GC_THRESHOLD) ? 1 : 0;
}

/*
 * is_block_live
 *
 * A block is LIVE if the inode that owns it (according to the
 * segment summary) still points back to it (according to the
 * current inode map + inode data).
 *
 * Returns 1 if live, 0 if dead.
 */
static int is_block_live(struct lfs_state *state,
                         uint32_t block,
                         uint32_t inode_no,
                         uint32_t block_idx)
{
    (void)block_idx;

    /* Check if any inode map entry points to this block */
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == block)
            return 1;
    }

    /* Check if any inode's data block points here */
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0) continue;
        struct lfs_inode inode;
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(state->inode_map[i], buf) != 0) continue;
        memcpy(&inode, buf, sizeof(inode));
        for (int j = 0; j < MAX_DIRECT_PTRS; j++) {
            if (inode.direct[j] == block)
                return 1;
        }
    }
    return 0;
}
/*
 * gc_collect
 *
 * Runs one full GC pass:
 *   - Finds the segment with most dead blocks
 *   - Copies live blocks forward
 *   - Zeroes the old segment
 *   - Resets log_tail to reclaim space
 */
int gc_collect(struct lfs_state *state)
{
    if (!state) return -1;

    printf("GC: starting collection, log_tail=%u\n", state->log_tail);

    int   best_seg       = -1;
    int   best_dead      = -1;

    /* --- Pass 1: find segment with most dead blocks --- */
    for (int seg = 0; seg < SEGMENT_COUNT; seg++) {
        uint32_t seg_start = (uint32_t)seg * BLOCKS_PER_SEGMENT;

        /* Skip segments that are before log start or beyond tail */
        if (seg_start < LOG_START_BLOCK)        continue;
        if (seg_start >= state->log_tail)       continue;

        /* Read segment summary */
        struct lfs_segment_summary sum;
        memset(&sum, 0, sizeof(sum));
        if (disk_read(seg_start, &sum) != 0)    continue;

        int dead = 0;
        for (int i = 1; i < BLOCKS_PER_SEGMENT; i++) {
            uint32_t block = seg_start + (uint32_t)i;
            if (block >= state->log_tail) break;

            if (!is_block_live(state, block,
                               sum.entry[i].inode_no,
                               sum.entry[i].block_idx))
                dead++;
        }

        if (dead > best_dead) {
            best_dead = dead;
            best_seg  = seg;
        }
    }

    if (best_seg < 0 || best_dead == 0) {
        printf("GC: nothing to collect\n");
        return 0;
    }

    uint32_t seg_start = (uint32_t)best_seg * BLOCKS_PER_SEGMENT;
    printf("GC: cleaning segment %d (start=%u, dead=%d)\n",
           best_seg, seg_start, best_dead);

    /* --- Pass 2: copy live blocks forward --- */
    struct lfs_segment_summary sum;
    memset(&sum, 0, sizeof(sum));
    disk_read(seg_start, &sum);

    for (int i = 1; i < BLOCKS_PER_SEGMENT; i++) {
        uint32_t block     = seg_start + (uint32_t)i;
        if (block >= state->log_tail) break;

        uint32_t inode_no  = sum.entry[i].inode_no;
        uint32_t block_idx = sum.entry[i].block_idx;

        if (!is_block_live(state, block, inode_no, block_idx))
            continue;   /* dead — skip */

        /* Read the live block */
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(block, buf) != 0) continue;

        /* Append it to the end of the log */
        int new_block = log_append_ex(state, buf, inode_no, block_idx);
        if (new_block < 0) {
            fprintf(stderr, "GC: log full during collection!\n");
            return -1;
        }

        /*
         * Update the inode map / inode to point to the new location.
         * Two cases:
         *   a) This block IS the inode  → update inode map
         *   b) This block is DATA       → update inode's direct ptr
         */
        if (state->inode_map[inode_no] == block) {
            /* Case a: inode block moved */
            state->inode_map[inode_no] = (uint32_t)new_block;
        } else {
            /* Case b: data block moved — update inode's direct ptr */
            struct lfs_inode inode;
            if (inode_read(state, inode_no, &inode) == 0) {
                if (block_idx < MAX_DIRECT_PTRS)
                    inode.direct[block_idx] = (uint32_t)new_block;
                inode_write(state, &inode);
            }
        }
    }

    /* --- Pass 3: zero out the old segment --- */
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, BLOCK_SIZE);
    for (int i = 0; i < BLOCKS_PER_SEGMENT; i++) {
        uint32_t block = seg_start + (uint32_t)i;
        if (block >= state->sb.total_blocks) break;
        disk_write(block, zero);
    }

    /*
     * If the cleaned segment was at the tail end of the log,
     * we can rewind the tail to reclaim space.
     * Otherwise the zeroed space will be reused naturally as
     * the log wraps (full wrap-around is a future enhancement).
     */
    if (seg_start + BLOCKS_PER_SEGMENT >= state->log_tail) {
        state->log_tail    = seg_start;
        state->sb.log_tail = seg_start;
        printf("GC: rewound log_tail to %u\n", state->log_tail);
    }

    log_checkpoint(state);
    printf("GC: done, log_tail=%u\n", state->log_tail);
    return 0;
}
