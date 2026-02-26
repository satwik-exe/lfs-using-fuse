/*
 * log.c — Log-Structured Filesystem write path
 *
 * In a true LFS, ALL writes go to the end of the log (never
 * update-in-place).  This file owns:
 *
 *   log_append()    — write one block to the next free position
 *                     and record it in the segment summary
 *   log_checkpoint()— persist inode map + superblock so the log
 *                     tail survives a remount
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

/*
 * get_segment_summary_block
 *
 * Returns the block number of the segment summary for the segment
 * that contains 'block'.
 * Each segment's first block is always its summary.
 */
static uint32_t get_summary_block(uint32_t block)
{
    uint32_t seg = block / BLOCKS_PER_SEGMENT;
    return seg * BLOCKS_PER_SEGMENT;
}

/*
 * log_append_with_summary
 *
 * Writes 'buf' to state->log_tail, records the owner info
 * (inode_no, block_idx) in the segment summary, advances the tail,
 * and returns the block number used.
 *
 * inode_no  = which inode owns this block (0 = metadata/unknown)
 * block_idx = index into inode->direct[] (0 for inode blocks)
 */
int log_append_ex(struct lfs_state *state, const void *buf,
                  uint32_t inode_no, uint32_t block_idx)
{
    if (!state || !buf) return -1;

    if (state->log_tail >= state->sb.total_blocks) {
        fprintf(stderr, "log_append: disk full (tail=%u, total=%u)\n",
                state->log_tail, state->sb.total_blocks);
        return -1;
    }

    uint32_t block = state->log_tail;

    /* Write the data block first */
    if (disk_write(block, buf) != 0) {
        fprintf(stderr, "log_append: disk_write failed at block %u\n",
                block);
        return -1;
    }

    /* Update the segment summary for this block */
    uint32_t sum_block = get_summary_block(block);
    uint32_t offset    = block - sum_block; /* position within segment */

    if (offset != 0) {
        /* Read existing summary, update our entry, write back */
        struct lfs_segment_summary sum;
        memset(&sum, 0, sizeof(sum));
        disk_read(sum_block, &sum);

        sum.entry[offset].inode_no  = inode_no;
        sum.entry[offset].block_idx = block_idx;

        disk_write(sum_block, &sum);
    }

    state->log_tail++;
    state->sb.log_tail = state->log_tail;

    return (int)block;
}

/*
 * log_append — convenience wrapper (metadata / unknown owner)
 */
int log_append(struct lfs_state *state, const void *buf)
{
    return log_append_ex(state, buf, 0, 0);
}

/*
 * log_checkpoint
 *
 * Makes the current log tail durable by:
 *   1. Writing the inode map to INODE_MAP_BLOCK
 *   2. Writing the updated superblock to block 0
 */
int log_checkpoint(struct lfs_state *state)
{
    if (!state) return -1;

    uint8_t imap_block[BLOCK_SIZE];
    memset(imap_block, 0, BLOCK_SIZE);
    memcpy(imap_block, state->inode_map,
           INODE_MAP_SIZE * sizeof(uint32_t));

    if (disk_write(INODE_MAP_BLOCK, imap_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write inode map\n");
        return -1;
    }

    uint8_t sb_block[BLOCK_SIZE];
    memset(sb_block, 0, BLOCK_SIZE);
    memcpy(sb_block, &state->sb, sizeof(state->sb));

    if (disk_write(0, sb_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write superblock\n");
        return -1;
    }

    return 0;
}
