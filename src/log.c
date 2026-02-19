/*
 * log.c — Log-Structured Filesystem write path
 *
 * In a true LFS, ALL writes go to the end of the log (never
 * update-in-place).  This file owns:
 *
 *   log_append()    — write one block to the next free position
 *   log_checkpoint()— persist inode map + superblock so the log
 *                     tail survives a remount
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

/*
 * log_append
 *
 * Writes 'buf' (exactly BLOCK_SIZE bytes) to state->log_tail,
 * advances the tail, and returns the block number that was used.
 * Returns -1 on error.
 *
 * This is the ONLY place that moves the log tail forward.
 */
int log_append(struct lfs_state *state, const void *buf)
{
    if (!state || !buf) return -1;

    /* Check we haven't run out of space */
    if (state->log_tail >= state->sb.total_blocks) {
        fprintf(stderr, "log_append: disk full (tail=%u, total=%u)\n",
                state->log_tail, state->sb.total_blocks);
        return -1;
    }

    uint32_t block = state->log_tail;

    if (disk_write(block, buf) != 0) {
        fprintf(stderr, "log_append: disk_write failed at block %u\n",
                block);
        return -1;
    }

    /* Advance tail in memory — caller must call log_checkpoint()
       to make this durable.                                         */
    state->log_tail++;
    state->sb.log_tail = state->log_tail;

    return (int)block;
}

/*
 * log_checkpoint
 *
 * Makes the current log tail durable by:
 *   1. Writing the inode map to INODE_MAP_BLOCK
 *   2. Writing the updated superblock to block 0
 *
 * Call this after every mutation (create, write, etc.) so that a
 * crash-and-remount lands back at a consistent state.
 */
int log_checkpoint(struct lfs_state *state)
{
    if (!state) return -1;

    /* --- 1. Write inode map --- */
    /*
     * The inode map is INODE_MAP_SIZE uint32_t values = 512 bytes,
     * which fits inside one 4 KB block easily.
     */
    uint8_t imap_block[BLOCK_SIZE];
    memset(imap_block, 0, BLOCK_SIZE);
    memcpy(imap_block, state->inode_map,
           INODE_MAP_SIZE * sizeof(uint32_t));

    if (disk_write(INODE_MAP_BLOCK, imap_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write inode map\n");
        return -1;
    }

    /* --- 2. Write superblock (always block 0) --- */
    uint8_t sb_block[BLOCK_SIZE];
    memset(sb_block, 0, BLOCK_SIZE);
    memcpy(sb_block, &state->sb, sizeof(state->sb));

    if (disk_write(0, sb_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write superblock\n");
        return -1;
    }

    return 0;
}
