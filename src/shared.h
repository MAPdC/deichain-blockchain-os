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

#ifndef SHARED_H
#define SHARED_H

#include <time.h>
#include <semaphore.h>
#include <sys/types.h>
#include <openssl/sha.h>
#include <stdio.h>

/*
 * System-Wide Constants
 * Maximum boundaries for blocks, transactions and string buffers
 * to ensure memory safety across shared segments.
 */
#define MAX_TRANSACTIONS_PER_BLOCK 64
#define MAX_HASH_SIZE (SHA256_DIGEST_LENGTH * 2 + 1)
#define TX_ID_SIZE 64
#define BLOCK_ID_SIZE 64

/*
 * Core Data Structures
 * Definitions for Transactions, Blocks and the Shared Memory layouts
 * (Transaction Pool, Blockchain Ledger and System Configuration).
 * Note: Arrays defined without explicit sizes are flexible array members
 * allocated dynamically based on configuration limits.
 */
typedef struct {
    char    id[TX_ID_SIZE];     /* "TX-<pid>-<counter>" */
    int     reward;             /* 1–3 initial; may grow via aging */
    int     sender_pid;         /* PID of the TxGen that created this tx */
    int     receiver_id;        /* random destination identifier */
    int     value;              /* random amount (stored as integer cents) */
    time_t  timestamp;          /* creation time */
    int     age;                /* incremented by Validator each pool touch */
    int     empty;              /* 1 = slot free, 0 = slot occupied */
} Transaction;

typedef struct {
    sem_t       pool_sem;           /* mutual exclusion for the whole pool */
    int         current_block_id;   /* monotonic counter used by Validator */
    Transaction transactions[];     /* flexible array; size = TX_POOL_SIZE */
} TransactionPool;

typedef struct {
    char        block_id[BLOCK_ID_SIZE];
    int         miner_id;
    int         transaction_count;
    time_t      timestamp;
    Transaction transactions[MAX_TRANSACTIONS_PER_BLOCK];
    int         nonce;
    int         difficulty;
    char        prev_hash[MAX_HASH_SIZE];
    char        curr_hash[MAX_HASH_SIZE];
} Block;

typedef struct {
    sem_t  ledger_sem;     /* mutual exclusion for the whole ledger */
    Block  blocks[];       /* flexible array; size = BLOCKCHAIN_BLOCKS */
} BlockchainLedger;

typedef struct {
    int   num_miners;
    int   tx_pool_size;
    int   transactions_per_block;
    int   blockchain_blocks;
    int   active_validators;
    sem_t config_sem;      /* protects active_validators */
} Config;


/*
 * IPC and Statistics Structures
 * Message queue payloads for inter-process communication (Validator -> Statistics)
 * and data structures for tracking miner performance.
 */
typedef struct {
    long   mtype;               /* always 1 */
    char   miner_id[64];
    char   block_id[BLOCK_ID_SIZE];
    int    valid;               /* 1 = accepted, 0 = rejected */
    int    credits;             /* sum of rewards in this block */
    time_t timestamp;
} StatsMessage;

typedef struct {
    int    valid_blocks;
    int    invalid_blocks;
    int    total_credits;
    time_t first_block_time;
    time_t last_block_time;
} MinerStats;

/*
 * Global State and Function Prototypes
 * Extern pointers for shared memory anchors and common utility functions
 * accessible by all system modules.
 */
extern Config           *shared_config;
extern TransactionPool  *tx_pool;
extern BlockchainLedger *ledger;

/* SHA-256 block hash (implemented in shared.c) */
void calculate_block_hash(const Block *block, char output[SHA256_DIGEST_LENGTH * 2 + 1]);

/* Log file handle (implemented in logging.c) */
FILE *log_file_handle(void);

#endif
