/**
 * DEIChain — Blockchain Simulation in C
 *
 * Authors:
 * - Miguel Cunha
 * - Miguel Fernandes
 *
 * Course: Operating Systems (2024/2025)
 * Degree: BSc in Informatics Engineering (LEI)
 * Institution: University of Coimbra - DEI
 */

#include <stdio.h>
#include <string.h>
#include <openssl/sha.h>
#include "shared.h"
#include "logging.h"

/*
 * Global Shared Memory Pointers
 * Pointers to the mapped shared memory segments.
 * These are initialized by each process upon startup.
 */
Config *shared_config = NULL;
TransactionPool *tx_pool = NULL;
BlockchainLedger *ledger = NULL;

/*
 * SHA-256 Block Hash Calculation
 * Serializes the block header and all its contained transactions into
 * a single string buffer, then computes the hex-encoded SHA-256 digest.
 */
void calculate_block_hash(const Block *block, char output[SHA256_DIGEST_LENGTH * 2 + 1]) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    char input[8192];
    int offset;

    offset = snprintf(input, sizeof(input),
        "prev_hash=%s|"
        "block_id=%s|"
        "miner_id=%d|"
        "timestamp=%ld|"
        "nonce=%d|"
        "difficulty=%d|"
        "tx_count=%d|",
        block->prev_hash,
        block->block_id,
        block->miner_id,
        (long)block->timestamp,
        block->nonce,
        block->difficulty,
        block->transaction_count);

    for (int i = 0; i < block->transaction_count; i++) {
        const Transaction *tx = &block->transactions[i];
        int written = snprintf(input + offset, sizeof(input) - (size_t)offset,
            "tx%d_id=%s|"
            "tx%d_sender=%d|"
            "tx%d_receiver=%d|"
            "tx%d_value=%d|"
            "tx%d_reward=%d|"
            "tx%d_time=%ld|",
            i, tx->id,
            i, tx->sender_pid,
            i, tx->receiver_id,
            i, tx->value,
            i, tx->reward,
            i, (long)tx->timestamp);

        if (written < 0) {
            break;
        }
        offset += written;
        if (offset >= (int)sizeof(input)) {
            break;
        }
    }

    SHA256((unsigned char*)input, (size_t)offset, hash);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(output + (i * 2), "%02x", hash[i]);
    }
    output[SHA256_DIGEST_LENGTH * 2] = '\0';
}

/*
 * Log File Handle Exposure
 * Provides access to the underlying log file descriptor.
 * Primarily used by the Controller to write structured ledger dumps.
 */
FILE *log_file_handle(void) {
    return log_get_file();
}