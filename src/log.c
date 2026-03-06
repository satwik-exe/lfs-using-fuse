/*
 * log.c — Log-Structured Filesystem write path
 *
 *   log_append()     — write one block to the next free log position
 *   log_checkpoint() — persist inode map + superblock + commit block
 *   log_recover()    — Stage 8: verify or repair log tail on mount
 */

#include <string.h>
#include <stdio.h>
#include "lfs.h"

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static uint32_t get_summary_block(uint32_t block)
{
    uint32_t seg = block / BLOCKS_PER_SEGMENT;
    return seg * BLOCKS_PER_SEGMENT;
}

/*
 * imap_crc
 *
 * Simple XOR checksum over all inode_map[] entries.
 * Cheap to compute and enough to detect a torn/partial write of the
 * inode map block.
 */
static uint32_t imap_crc(const uint32_t *imap)
{
    uint32_t crc = 0;
    for (int i = 0; i < INODE_MAP_SIZE; i++)
        crc ^= imap[i];
    return crc;
}

/* ------------------------------------------------------------------ */
/*  log_append_ex                                                       */
/* ------------------------------------------------------------------ */

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

    if (disk_write(block, buf) != 0) {
        fprintf(stderr, "log_append: disk_write failed at block %u\n",
                block);
        return -1;
    }

    /* Update segment summary */
    uint32_t sum_block = get_summary_block(block);
    uint32_t offset    = block - sum_block;

    if (offset != 0) {
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

int log_append(struct lfs_state *state, const void *buf)
{
    return log_append_ex(state, buf, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  log_checkpoint                                                      */
/* ------------------------------------------------------------------ */

/*
 * log_checkpoint — make the current state fully durable.
 *
 * Write order (each step must complete before the next):
 *   1. Inode map   → INODE_MAP_BLOCK  (block 1)
 *   2. Superblock  → block 0          (with incremented commit_seq)
 *   3. Commit block→ COMMIT_BLOCK     (block 2)
 *
 * The commit block is written LAST.  On recovery, if the commit
 * block's seq matches the superblock's seq, we know all three writes
 * completed and the checkpoint is valid.  If they don't match (or the
 * commit magic is wrong), recovery discards the partial checkpoint.
 */
int log_checkpoint(struct lfs_state *state)
{
    if (!state) return -1;

    /* Step 1 — inode map */
    uint8_t imap_block[BLOCK_SIZE];
    memset(imap_block, 0, BLOCK_SIZE);
    memcpy(imap_block, state->inode_map,
           INODE_MAP_SIZE * sizeof(uint32_t));

    if (disk_write(INODE_MAP_BLOCK, imap_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write inode map\n");
        return -1;
    }

    /* Step 2 — superblock with incremented sequence number */
    state->sb.commit_seq++;
    state->sb.log_tail = state->log_tail;

    uint8_t sb_block[BLOCK_SIZE];
    memset(sb_block, 0, BLOCK_SIZE);
    memcpy(sb_block, &state->sb, sizeof(state->sb));

    if (disk_write(0, sb_block) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write superblock\n");
        return -1;
    }

    /* Step 3 — commit block (written last — this is the "seal") */
    struct lfs_commit commit;
    memset(&commit, 0, sizeof(commit));
    commit.commit_magic = LFS_COMMIT_MAGIC;
    commit.commit_seq   = state->sb.commit_seq;
    commit.log_tail     = state->log_tail;
    commit.imap_crc     = imap_crc(state->inode_map);

    if (disk_write(COMMIT_BLOCK, &commit) != 0) {
        fprintf(stderr, "log_checkpoint: failed to write commit block\n");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  log_recover  (Stage 8)                                              */
/* ------------------------------------------------------------------ */

/*
 * log_recover — called once at mount time, before normal operation.
 *
 * Checks whether the last checkpoint completed fully by comparing
 * the commit block against the superblock.  Three possible outcomes:
 *
 * A) Commit is valid (magic ok, seq matches, CRC matches):
 *    The last checkpoint was clean.  Trust superblock.log_tail as-is.
 *
 * B) Commit seq or CRC mismatches (crash between sb write and commit):
 *    The inode map or commit block may be from an earlier checkpoint.
 *    Rewind log_tail by scanning from log_start for the last non-zero
 *    block, then rebuild the inode map from inode blocks found in log.
 *
 * C) Commit magic is wrong (very early crash, before any checkpoint):
 *    Treat as case B.
 *
 * After recovery the in-memory state is consistent and a fresh
 * checkpoint is written to seal the recovered state.
 */
int log_recover(struct lfs_state *state)
{
    if (!state) return -1;

    printf("log_recover: checking commit block...\n");

    /* Read the commit block */
    struct lfs_commit commit;
    memset(&commit, 0, sizeof(commit));
    disk_read(COMMIT_BLOCK, &commit);

    /* Compute expected CRC from the inode map we loaded at mount */
    uint32_t expected_crc = imap_crc(state->inode_map);

    int commit_ok = (commit.commit_magic == LFS_COMMIT_MAGIC)
                 && (commit.commit_seq   == state->sb.commit_seq)
                 && (commit.imap_crc     == expected_crc)
                 && (commit.log_tail     == state->sb.log_tail);

    if (commit_ok) {
        printf("log_recover: commit valid (seq=%u, tail=%u) — no recovery needed\n",
               commit.commit_seq, commit.log_tail);
        return 0;
    }

    /*
     * Something doesn't match — the last checkpoint was interrupted.
     * Log what we found to help with debugging.
     */
    printf("log_recover: INCOMPLETE CHECKPOINT DETECTED\n");
    printf("  superblock: seq=%u tail=%u\n",
           state->sb.commit_seq, state->sb.log_tail);
    printf("  commit blk: magic=0x%x seq=%u tail=%u crc=0x%x\n",
           commit.commit_magic, commit.commit_seq,
           commit.log_tail, commit.imap_crc);
    printf("  imap  crc : expected=0x%x\n", expected_crc);

    /*
     * Step 1: Find the true end of the log.
     *
     * Scan backwards from superblock.log_tail - 1 toward log_start.
     * The first non-zero block we find is the last block that was
     * actually written.  Set log_tail = that block + 1.
     *
     * We use "any non-zero byte" as the test because a block that was
     * never written will be all zeros from mkfs's ftruncate.
     */
    uint8_t buf[BLOCK_SIZE];
    uint32_t scan_end = state->sb.log_tail;
    if (scan_end > state->sb.total_blocks)
        scan_end = state->sb.total_blocks;

    uint32_t true_tail = LOG_START_BLOCK;  /* fallback: empty log */

    for (uint32_t b = scan_end; b > LOG_START_BLOCK; b--) {
        memset(buf, 0, BLOCK_SIZE);
        disk_read(b - 1, buf);
        int nonzero = 0;
        for (int i = 0; i < BLOCK_SIZE; i++) {
            if (buf[i] != 0) { nonzero = 1; break; }
        }
        if (nonzero) {
            true_tail = b;   /* b-1 is the last written block */
            break;
        }
    }

    printf("log_recover: rewinding log_tail %u -> %u\n",
           state->sb.log_tail, true_tail);

    state->log_tail     = true_tail;
    state->sb.log_tail  = true_tail;

    /*
     * Step 2: Rebuild the inode map from scratch.
     *
     * Scan every block in the live log range.  For each block that
     * looks like a valid inode (inode_no < INODE_MAP_SIZE, type is
     * FILE or DIR), update inode_map[inode_no] to point to this block
     * if it's newer (higher block number = more recent in the log).
     *
     * This gives us the most recent version of every inode.
     */
    printf("log_recover: rebuilding inode map from log blocks [%u, %u)\n",
           LOG_START_BLOCK, true_tail);

    memset(state->inode_map, 0, sizeof(state->inode_map));

    for (uint32_t b = LOG_START_BLOCK; b < true_tail; b++) {
        memset(buf, 0, BLOCK_SIZE);
        if (disk_read(b, buf) != 0) continue;

        struct lfs_inode *candidate = (struct lfs_inode *)buf;

        /* Heuristic: valid inode has sane inode_no and known type */
        if (candidate->inode_no == 0 &&
            candidate->type != INODE_TYPE_DIR)
            continue;   /* inode_no 0 is root — only DIR is valid   */

        if (candidate->inode_no >= INODE_MAP_SIZE)
            continue;

        if (candidate->type != INODE_TYPE_FILE &&
            candidate->type != INODE_TYPE_DIR)
            continue;

        /* Later block = more recent version — always prefer it */
        uint32_t ino = candidate->inode_no;
        if (b > state->inode_map[ino] || state->inode_map[ino] == 0)
            state->inode_map[ino] = b;
    }

    /* Root inode (0) must always be present */
    if (state->inode_map[0] == 0) {
        fprintf(stderr, "log_recover: ERROR — root inode not found "
                        "after recovery!\n");
        return -1;
    }

    printf("log_recover: inode map rebuilt, %d inodes found\n",
           (int)(sizeof(state->inode_map) / sizeof(state->inode_map[0])));

    /*
     * Step 3: Seal the recovered state with a fresh checkpoint.
     * This overwrites the bad superblock/commit so future mounts
     * see a clean state immediately.
     */
    printf("log_recover: writing recovery checkpoint...\n");
    if (log_checkpoint(state) != 0) {
        fprintf(stderr, "log_recover: checkpoint after recovery failed\n");
        return -1;
    }

    printf("log_recover: recovery complete, log_tail=%u\n",
           state->log_tail);
    return 0;
}
