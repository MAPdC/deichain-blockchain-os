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
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <signal.h>
#include <sys/stat.h>
#include <openssl/sha.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "shared.h"
#include "logging.h"

/*
 * Configuration and Global State
 * Macro definitions, fixed parameters, and global variables
 * shared throughout the miner process.
 */
#define VALIDATOR_PIPE "VALIDATOR_PIPE"
#define POW_MAX_OPS 5000000
#define IDLE_SLEEP_US 200000

static int pipe_fd = -1;
static volatile sig_atomic_t g_stop = 0;

/*
 * Shared Memory Accessors
 * Functions to map and access the shared memory segments
 * (configuration, transaction pool, and blockchain ledger).
 */
static Config* access_shared_config(const char* shm_name) {
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    shared_config = mmap(NULL, sizeof(Config), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (shared_config == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    return shared_config;
}

static TransactionPool* access_shared_tx_pool(const char* shm_name) {
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    tx_pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (tx_pool == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    return tx_pool;
}

static BlockchainLedger* access_shared_ledger(const char* shm_name) {
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
    
    int fd = shm_open(shm_name, O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open ledger");
        exit(EXIT_FAILURE);
    }

    ledger = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (ledger == MAP_FAILED) {
        perror("mmap ledger");
        exit(EXIT_FAILURE);
    }

    return ledger;
}

/*
 * Proof of Work (PoW) Engine
 * Calculates block difficulty based on transaction rewards and
 * performs the hashing iterations to find a valid nonce.
 */
static int solve_pow(Block *block, int difficulty) {
    block->nonce = 0;
    block->difficulty = difficulty;
    
    for (long ops = 0; ops < POW_MAX_OPS; ops++) {
        calculate_block_hash((const Block*)block, block->curr_hash);
        int valid = 1;
        for (int i = 0; i < difficulty; i++) {
            if (block->curr_hash[i] != '0') {
                valid = 0;
                break;
            }
        }
        
        if (valid) return 1;
        block->nonce++;
    }
    return 0;
}

static int calculate_difficulty(int max_reward) {
    int d = max_reward;
    if (d < 1) {
        d = 1;
    }
    if (d > 6) {
        d = 6;
    }
    return d;
}

/*
 * Ledger Utilities
 * Helper functions to check ledger capacity and safely retrieve
 * the hash of the most recently validated block.
 */
static int ledger_is_full(void) {
    sem_wait(&ledger->ledger_sem);
    
    int full = (ledger->blocks[shared_config->blockchain_blocks - 1].curr_hash[0] != '\0');
    
    sem_post(&ledger->ledger_sem);
    return full;
}

static void get_prev_hash(char *out, size_t out_size) {
    sem_wait(&ledger->ledger_sem);

    int last = -1;
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] != '\0') {
            last = i;
        } else {
            break;
        }
    }

    if (last == -1) {
        strncpy(out, "INITIAL_HASH", out_size - 1);
    } else {
        strncpy(out, ledger->blocks[last].curr_hash, out_size - 1);
    }
    out[out_size - 1] = '\0';

    sem_post(&ledger->ledger_sem);
}

/*
 * Inter-Process Communication (IPC)
 * Functions handling the named pipe (FIFO) communication
 * to send successfully mined blocks to the Validator.
 */
static void open_validator_pipe(void) {

    pipe_fd = open(VALIDATOR_PIPE, O_WRONLY);
    if (pipe_fd == -1) {
        log_message("MINER", "Error opening the pipe %s: %s", VALIDATOR_PIPE, strerror(errno));
        exit(EXIT_FAILURE);
    }

    log_message("MINER", "Named pipe opened successfully");
}

static void send_block_to_validator(Block *block) {
    if (pipe_fd == -1) {
        return;
    }
    
    ssize_t written = write(pipe_fd, block, sizeof(Block));
    if (written != (ssize_t)sizeof(Block)) {
        log_message("MINER",  "Warning: partial write to pipe (%zd/%zu bytes)", written, sizeof(Block));
    }
}

/*
 * Miner Thread Routine
 * Core execution loop for each miner thread: selects transactions,
 * performs PoW, and submits the resulting block.
 */
typedef struct {
    int miner_id;
} MinerData;

static void *miner_thread(void *arg) {
    MinerData *data = (MinerData *)arg;
    log_message("MINER", "THREAD MINER %d CREATED", data->miner_id);

    int counter = 0;
    
    while (!g_stop) {

        if (ledger_is_full()) {
            log_message("MINER", "Thread %d: ledger is full, closing miner thread.", data->miner_id);
            break;
        }

        Block new_block;
        memset(&new_block, 0, sizeof(Block));
        new_block.miner_id = data->miner_id;
        new_block.timestamp = time(NULL);
        new_block.transaction_count = 0;
        
        snprintf(new_block.block_id, sizeof(new_block.block_id), "MINER-%d-%d", data->miner_id, counter);

        get_prev_hash(new_block.prev_hash, sizeof(new_block.prev_hash));

        /* Select transactions */
        sem_wait(&tx_pool->pool_sem);
        
        int pool_size = shared_config->tx_pool_size;
        int tpb = shared_config->transactions_per_block;

        /* Build an index of non-empry slots */
        int *idx = malloc(pool_size * sizeof(int));
        if (!idx) {
            sem_post(&tx_pool->pool_sem);
            sleep(1);
            continue;
        }

        int count = 0;
        for (int i = 0; i < pool_size; i++) {
            if (!tx_pool->transactions[i].empty) {
                idx[count++] = i;
            }
        }

        if (count < tpb) {
            /* Not enough transactions yet*/
            free(idx);
            sem_post(&tx_pool->pool_sem);
            usleep(IDLE_SLEEP_US);
            continue;
        }

        /* Sort indices by reward descending (inline comparator on pool) */
        /* Simple insertion sort to avoid auxiliary array allocation */
        for (int i = 1; i < count; i++) {
            int key = idx[i];
            int key_reward = tx_pool->transactions[key].reward;
            int j = i - 1;
            while (j >= 0 && tx_pool->transactions[idx[j]].reward < key_reward) {
                idx[j + 1] = idx[j];
                j--;
            }
            idx[j + 1] = key;
        }

        int max_reward = 0;
        for (int i = 0; i < tpb; i++) {
            new_block.transactions[i] = tx_pool->transactions[idx[i]];
            new_block.transaction_count++;
            if (tx_pool->transactions[idx[i]].reward > max_reward) {
                max_reward = tx_pool->transactions[idx[i]].reward;
            }
        }
        free(idx);
        sem_post(&tx_pool->pool_sem);


        int difficulty = calculate_difficulty(max_reward);

        log_message("MINER", "MINER %d: STARTED MINING BLOCK %s (diff=%d)", data->miner_id, new_block.block_id, difficulty);

        int pow_ok = solve_pow(&new_block, difficulty);
        if (!pow_ok) {
            log_message("MINER", "MINER %d: POW_MAX_OPS reached, block was discarded", data->miner_id);
            continue;
        }

        log_message("MINER", "MINER %d: FINISHED MINING BLOCK %s (nonce=%d)", data->miner_id, new_block.block_id, new_block.nonce);

        send_block_to_validator(&new_block);
        counter++;
    }

    return NULL;
}

/*
 * Signal Handling
 * Gracefully intercepts termination signals (SIGINT, SIGTERM)
 * to stop miner threads and cleanly exit the process.
 */
static void handle_sigterm(int sig) {
    (void)sig;
    g_stop = 1;
}

/*
 * Main Entry Point
 * Process initialization, dynamic thread pool creation, and
 * teardown/cleanup of resources upon termination.
 */
int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Use: %s <config_shm> <tx_pool_shm> <ledger_shm>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    struct sigaction sa = { .sa_handler = handle_sigterm, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    init_logger();

    shared_config = access_shared_config(argv[1]);
    tx_pool = access_shared_tx_pool(argv[2]);
    ledger = access_shared_ledger(argv[3]);

    open_validator_pipe();
    log_message("MINER", "Miner process started with %d threads.", shared_config->num_miners);

    int n = shared_config->num_miners;
    pthread_t *threads = malloc(n * sizeof(pthread_t));
    MinerData *tdata = malloc(n * sizeof(MinerData));
    if (!threads || !tdata) {
        log_message("MINER", "Error allocating memory for threads");
    }
    
    for (int i = 0; i < n; i++) {
        tdata[i].miner_id = i + 1;
        
        if (pthread_create(&threads[i], NULL, miner_thread, &tdata[i]) != 0) {
            log_message("MINER", "Error creating miner thread: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    for (int i = 0; i < n; i++) {
        pthread_join(threads[i], NULL);
    }

    free(threads);
    free(tdata);

    if (pipe_fd != -1) {
        close(pipe_fd);
    }

    size_t tx_pool_size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    size_t ledger_size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
    munmap(shared_config, sizeof(Config));
    munmap(tx_pool, tx_pool_size);
    munmap(ledger, ledger_size);
    
    close_logger();
    return 0;
}
