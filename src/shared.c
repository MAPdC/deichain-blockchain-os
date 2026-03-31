/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#include "shared.h"

Config *shared_config = NULL;
TransactionPool *tx_pool = NULL;
BlockchainLedger *ledger = NULL;

#include <stdio.h>
#include "shared.h"
#include <openssl/sha.h>
#include <string.h>

void calculate_block_hash(const Block *block, char output[SHA256_DIGEST_LENGTH*2+1]) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    char input[4096];

    // Construção CONSISTENTE da string de input
    int offset = snprintf(input, sizeof(input),
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
        block->timestamp,
        block->nonce,
        block->difficulty,
        block->transaction_count);

    // Adicionar cada transação de forma explícita
    for (int i = 0; i < block->transaction_count; i++) {
        const Transaction *tx = &block->transactions[i];
        offset += snprintf(input + offset, sizeof(input) - offset,
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
            i, tx->timestamp);
    }

    // Cálculo do hash SHA-256
    SHA256((unsigned char*)input, strlen(input), hash);

    // Conversão para hexadecimal
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(output + (i*2), "%02x", hash[i]);
    }
    output[SHA256_DIGEST_LENGTH*2] = '\0';
}