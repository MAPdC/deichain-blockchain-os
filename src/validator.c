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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <openssl/sha.h>
#include <sys/msg.h>
#include <signal.h>
#include <sys/mman.h>
#include <errno.h>
#include "shared.h"
#include "logging.h"

#define CONFIG_FILE "config.cfg"
#define VALIDATOR_PIPE "VALIDATOR_PIPE"

/*
 * Configuration and Global State
 * File descriptors, IPC identifiers, and graceful shutdown flags
 * utilized exclusively by the Validator process.
 */

static int pipe_fd = -1;
static int msgid = -1;
static volatile sig_atomic_t g_stop = 0;

/*
 * Shared Memory Accessors
 * Helper functions to map and attach to the configuration,
 * transaction pool, and blockchain ledger shared memory segments.
 */
static Config *access_shared_config(const char *name) {
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    Config *c = mmap(NULL, sizeof(Config), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    
    if (c == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    return c;
}

static TransactionPool *access_shared_tx_pool(const char *name, int sz) {
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * sz;
    
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    TransactionPool *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    
    if (p == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    return p;
}

static BlockchainLedger *access_shared_ledger(const char *name, int sz) {
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * sz;
    
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open ledger");
        exit(EXIT_FAILURE);
    }

    BlockchainLedger *l = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    
    if (l == MAP_FAILED) {
        perror("mmap ledger");
        exit(EXIT_FAILURE);
    }

    return l;
}

/*
 * Validation Utilities
 * Functions to verify Proof of Work (SHA-256) and safely locate
 * the most recent valid block in the ledger.
 */
static int find_last_valid_block_locked(void) {
    int last = -1;
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] == '\0') {
            break;
        }
        last = i;
    }
    return last;
}

static int validate_pow(const Block *block) {
    char computed[SHA256_DIGEST_LENGTH * 2 + 1];
    calculate_block_hash(block, computed);
    
    if (strcmp(computed, block->curr_hash) != 0) {
        log_message("VALIDATOR", "Hash mismatch: Calculated=%s vs Declared=%s", computed, block->curr_hash);
        return 0;
    }

    for (int i = 0; i < block->difficulty; i++) {
        if (block->curr_hash[i] != '0') return 0;
    }
    
    return 1;
}

/*
 * Statistics Reporting
 * Constructs and dispatches validation outcomes (valid/invalid)
 * and reward credits to the Statistics process via message queue.
 */
static void send_stats(const Block *block, int valid) {
    StatsMessage msg = {
        .mtype     = 1,
        .valid     = valid,
        .timestamp = time(NULL),
        .credits   = 0
    };
    snprintf(msg.miner_id, sizeof(msg.miner_id), "%d", block->miner_id);
    strncpy(msg.block_id, block->block_id, sizeof(msg.block_id) - 1);
    for (int i = 0; i < block->transaction_count; i++) {
        msg.credits += block->transactions[i].reward;
    }

    if (msgsnd(msgid, &msg, sizeof(StatsMessage) - sizeof(long), 0) == -1) {
        log_message("VALIDATOR", "msgsnd failed: %s", strerror(errno));
    }
}

/*
 * Core Block Validation
 * Atomically validates PoW, chain continuity, and transaction availability.
 * Appends valid blocks to the ledger and purges processed transactions.
 */
static void process_block(Block *block) {

    log_message("VALIDATOR", "STARTED VALIDATING BLOCK FROM MINER %d (%s)", block->miner_id, block->block_id);

    /* 1. PoW */
    if (!validate_pow(block)) {
        log_message("VALIDATOR", "BLOCK FROM MINER %d INVALID (PoW)", block->miner_id);
        send_stats(block, 0);
        return;
    }

    /* 2. Chain continuity + append — held under ledger_sem to be atomic */
    sem_wait(&ledger->ledger_sem);

    int last = find_last_valid_block_locked();

    if (last == -1) {
        /* Genesis block */
        if (strcmp(block->prev_hash, "INITIAL_HASH") != 0) {
            sem_post(&ledger->ledger_sem);
            log_message("VALIDATOR", "BLOCK FROM MINER %d INVALID (prev_hash genesis)", block->miner_id);
            send_stats(block, 0);
            return;
        }
    } else {
        if (strcmp(block->prev_hash, ledger->blocks[last].curr_hash) != 0) {
            sem_post(&ledger->ledger_sem);
            log_message("VALIDATOR", "BLOCK FROM MINER %d INVALID (prev_hash mismatch)", block->miner_id);
            send_stats(block, 0);
            return;
        }
    }

    int next_idx = last + 1;
    if (next_idx >= shared_config->blockchain_blocks) {
        sem_post(&ledger->ledger_sem);
        log_message("VALIDATOR", "Ledger full — terminating simulation");
        g_stop = 1;
        return;
    }

    /* 3. Transactions still in pool? — check while ledger is locked so no
     * other validator can slip in and remove them between our check and write. */
    sem_wait(&tx_pool->pool_sem);

    int tx_valid = 1;
    for (int i = 0; i < block->transaction_count && tx_valid; i++) {
        int found = 0;
        for (int j = 0; j < shared_config->tx_pool_size; j++) {
            if (!tx_pool->transactions[j].empty && strcmp(tx_pool->transactions[j].id, block->transactions[i].id) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            tx_valid = 0;
        }
    }

    if (!tx_valid) {
        sem_post(&tx_pool->pool_sem);
        sem_post(&ledger->ledger_sem);
        log_message("VALIDATOR", "BLOCK FROM MINER %d INVALID (tx already processed)", block->miner_id);
        send_stats(block, 0);
        return;
    }

    /* All checks passed — commit atomically */
    ledger->blocks[next_idx] = *block;

    /* Remove transactions from pool */
    for (int i = 0; i < block->transaction_count; i++) {
        for (int j = 0; j < shared_config->tx_pool_size; j++) {
            if (!tx_pool->transactions[j].empty && strcmp(tx_pool->transactions[j].id, block->transactions[i].id) == 0) {
                tx_pool->transactions[j].empty = 1;
                break;
            }
        }
    }

    sem_post(&tx_pool->pool_sem);
    sem_post(&ledger->ledger_sem);

    log_message("VALIDATOR", "BLOCK FROM MINER %d VALID!", block->miner_id);
    log_message("VALIDATOR", "BLOCK FROM MINER %d INSERTED IN BLOCKCHAIN (pos %d)", block->miner_id, next_idx);
    send_stats(block, 1);
}

/*
 * Anti-Starvation (Aging) Mechanism
 * Increments the age of pending transactions upon every pool inspection,
 * periodically boosting their reward to ensure eventual processing.
 */
static void age_transactions(void) {
    sem_wait(&tx_pool->pool_sem);
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            tx_pool->transactions[i].age++;
            if (tx_pool->transactions[i].age % 50 == 0) {
                tx_pool->transactions[i].reward++; /* anti-starvation */
            }
        }
    }
    sem_post(&tx_pool->pool_sem);
}

/*
 * Resource Cleanup (Teardown)
 * Safely detaches shared memory segments, closes the named pipe,
 * and performs final teardown upon process termination.
 */
static void cleanup(void) {
    log_message("VALIDATOR", "Free resources...");
    
    if (pipe_fd != -1) {
        close(pipe_fd);
    }
    
    if (shared_config && tx_pool) {
        size_t sz = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
        munmap(tx_pool, sz);
    }
    if (shared_config && ledger) {
        size_t sz = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
        munmap(ledger, sz);
    }
    if (shared_config) munmap(shared_config, sizeof(Config));

    close_logger();
    exit(EXIT_SUCCESS);
}

/*
 * Signal Handling
 * Intercepts SIGINT/SIGTERM to safely break the validation loop
 * and trigger the graceful teardown sequence.
 */
static void handle_sigterm(int sig) {
    (void)sig;
    g_stop = 1;
}

/*
 * Main Entry Point
 * Connects to IPC mechanisms (pipe, message queue, shared memory)
 * and enters the continuous block validation and transaction aging loop.
 */
int main(void) {
    struct sigaction sa = { .sa_handler = handle_sigterm, .sa_flags = SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    init_logger();

    shared_config = access_shared_config("/deichain_config");
    tx_pool = access_shared_tx_pool("/deichain_tx_pool", shared_config->tx_pool_size);
    ledger = access_shared_ledger("/deichain_ledger", shared_config->blockchain_blocks);

    log_message("VALIDATOR", "READY FOR WORK");

    pipe_fd = open(VALIDATOR_PIPE, O_RDONLY);
    if (pipe_fd == -1) {
        log_message("VALIDATOR", "Error opening pipe: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }

    key_t key = ftok(CONFIG_FILE, 'M');
    if (key == -1) {
        perror("ftok"); 
        exit(EXIT_FAILURE); 
    }
    msgid = msgget(key, 0666 | IPC_CREAT);
    if (msgid == -1) { 
        perror("msgget"); 
        exit(EXIT_FAILURE); 
    }

    while (!g_stop) {
        age_transactions();

        Block block;
        ssize_t n = read(pipe_fd, &block, sizeof(Block));

        if (n == (ssize_t)sizeof(Block)) {
            process_block(&block);
        } else if (n == 0) {
            /* Pipe write-end closed (miner exited) */
            log_message("VALIDATOR", "Pipe fechado — terminando");
            break;
        } else if (n == -1) {
            if (errno == EINTR) {
                continue;   /* interrupted by signal, retry */
            }
            perror("read pipe");
            break;
        }
    }

    cleanup();
    return 0;
}
