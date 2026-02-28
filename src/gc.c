/*
 * gc.c — Garbage Collector for LFS
 *
 * Strategy: build relocation table, compact forward, fix all pointers.
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

int gc_should_run(struct lfs_state *state)
{
    if (!state) return 0;
    return ((state->sb.total_blocks - state->log_tail) < GC_THRESHOLD) ? 1 : 0;
}

int gc_collect(struct lfs_state *state)
{
    if (!state) return -1;

    uint32_t old_tail = state->log_tail;
    printf("GC: starting, log_tail=%u free=%u\n",
           old_tail, state->sb.total_blocks - old_tail);

    /*
     * Step 1: mark which blocks are live using a bitmap.
     */
    uint8_t live[TOTAL_BLOCKS];
    memset(live, 0, sizeof(live));

    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0) continue;
        uint32_t iblk = state->inode_map[i];
        if (iblk < TOTAL_BLOCKS) live[iblk] = 1;

        uint8_t buf[BLOCK_SIZE];
        if (disk_read(iblk, buf) != 0) continue;
        struct lfs_inode *inode = (struct lfs_inode *)buf;
        for (int j = 0; j < MAX_DIRECT_PTRS; j++) {
            if (inode->direct[j] != 0 && inode->direct[j] < TOTAL_BLOCKS)
                live[inode->direct[j]] = 1;
        }
    }

    /* Count dead blocks */
    int dead = 0;
    for (uint32_t b = LOG_START_BLOCK; b < old_tail; b++)
        if (!live[b]) dead++;
    printf("GC: %d dead blocks out of %u used\n", dead,
           old_tail - LOG_START_BLOCK);

    if (dead == 0) { printf("GC: nothing to collect\n"); return 0; }

    /*
     * Step 2: forward compaction.
     * dst = next free (dead) slot, scanning left to right.
     * src = next live block, scanning left to right.
     * Move live[src] into dst, record relocation.
     */
    uint32_t relo_old[TOTAL_BLOCKS];
    uint32_t relo_new[TOTAL_BLOCKS];
    int nrelo = 0;

    uint8_t tmp[BLOCK_SIZE];
    uint8_t zero[BLOCK_SIZE];
    memset(zero, 0, BLOCK_SIZE);

    uint32_t dst = LOG_START_BLOCK;
    for (uint32_t src = LOG_START_BLOCK; src < old_tail; src++) {
        if (!live[src]) continue;      /* dead — skip */
        /* advance dst to next dead slot */
        while (dst < src && live[dst]) dst++;
        if (dst >= src) { dst++; continue; } /* already compact here */

        /* move src -> dst */
        disk_read(src, tmp);
        disk_write(dst, tmp);
        disk_write(src, zero);
        live[dst] = 1;
        live[src] = 0;
        relo_old[nrelo] = src;
        relo_new[nrelo] = dst;
        nrelo++;
        dst++;
    }

    printf("GC: moved %d blocks\n", nrelo);

    /*
     * Step 3: apply all relocations to inode_map and direct[] pointers.
     * For each inode, apply ALL pending relocations in one read-modify-write.
     */

    /* First update inode_map entries (inode blocks that moved) */
    for (int r = 0; r < nrelo; r++) {
        for (int i = 0; i < INODE_MAP_SIZE; i++) {
            if (state->inode_map[i] == relo_old[r])
                state->inode_map[i] = relo_new[r];
        }
    }

    /* Then update direct[] pointers inside each inode */
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] == 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(state->inode_map[i], buf) != 0) continue;
        struct lfs_inode *inode = (struct lfs_inode *)buf;
        int dirty = 0;
        for (int j = 0; j < MAX_DIRECT_PTRS; j++) {
            for (int r = 0; r < nrelo; r++) {
                if (inode->direct[j] == relo_old[r]) {
                    inode->direct[j] = relo_new[r];
                    dirty = 1;
                    break;
                }
            }
        }
        if (dirty) disk_write(state->inode_map[i], buf);
    }

    /*
     * Step 4: rewind log_tail to just after the highest live block.
     */
    uint32_t highest = LOG_START_BLOCK;
    for (uint32_t b = old_tail - 1; b >= LOG_START_BLOCK; b--) {
        if (live[b]) { highest = b; break; }
        if (b == LOG_START_BLOCK) break;
    }
    /* Find highest by scanning all inode pointers (most accurate) */
    highest = LOG_START_BLOCK;
    for (int i = 0; i < INODE_MAP_SIZE; i++) {
        if (state->inode_map[i] > highest) highest = state->inode_map[i];
        if (state->inode_map[i] == 0) continue;
        uint8_t buf[BLOCK_SIZE];
        if (disk_read(state->inode_map[i], buf) != 0) continue;
        struct lfs_inode *in = (struct lfs_inode *)buf;
        for (int j = 0; j < MAX_DIRECT_PTRS; j++)
            if (in->direct[j] > highest) highest = in->direct[j];
    }

    uint32_t new_tail = highest + 1;
    if (new_tail % BLOCKS_PER_SEGMENT != 0)
        new_tail = ((new_tail / BLOCKS_PER_SEGMENT) + 1) * BLOCKS_PER_SEGMENT;
    if (new_tail > old_tail) new_tail = old_tail;

    printf("GC: rewound log_tail %u -> %u (reclaimed %u blocks)\n",
           old_tail, new_tail, old_tail - new_tail);
    state->log_tail    = new_tail;
    state->sb.log_tail = new_tail;

    log_checkpoint(state);
    printf("GC: done, log_tail=%u free=%u\n",
           state->log_tail, state->sb.total_blocks - state->log_tail);
    return 0;
}
