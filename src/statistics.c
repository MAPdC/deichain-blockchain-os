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
#include <sys/msg.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "shared.h"
#include "logging.h"

#define CONFIG_FILE "config.cfg"
#define SHM_CONFIG_NAME "/deichain_config"
#define SHM_TX_POOL_NAME "/deichain_tx_pool"
#define SHM_LEDGER_NAME "/deichain_ledger"
#define MAX_MINERS 64

static MinerStats miners[MAX_MINERS + 1];
static int total_blocks = 0;
static int total_valid_blocks = 0;
static int msgid = -1;
static int total_processing_us = 0;
static int processed_tx_count = 0;

static volatile sig_atomic_t g_print_stats = 0;
static volatile sig_atomic_t g_stop = 0;

/*
 * Shared Memory Accessors
 * Functions to map and access the shared memory segments
 * (configuration, transaction pool, and blockchain ledger).
 */
static Config *access_shared_config(void) {
    int fd = shm_open(SHM_CONFIG_NAME, O_RDWR, 0666);
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

static TransactionPool *access_shared_tx_pool(int sz) {
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * sz;
    
    int fd = shm_open(SHM_TX_POOL_NAME, O_RDWR, 0666);
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

static BlockchainLedger *access_shared_ledger(int sz) {
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * sz;
    
    int fd = shm_open(SHM_LEDGER_NAME, O_RDWR, 0666);
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
 * Statistics Output and Formatting
 * Aggregates and displays current system metrics, including miner
 * performance, block processing rates, and pool occupancy.
 */
static void print_statistics(void) {

    log_message("STATISTICS", "===== Current Statistics =====");

    for (int i = 1; i <= shared_config->num_miners && i <= MAX_MINERS; i++) {
        if (miners[i].valid_blocks == 0 && miners[i].invalid_blocks == 0) {
            continue;
        }

        int total = miners[i].valid_blocks + miners[i].invalid_blocks;
        double success_rate = total > 0 ? (miners[i].valid_blocks * 100.0 / total) : 0.0;

        log_message("STATISTICS",
                    "Miner %d: Valid=%d | Invalid=%d | Credits=%d | Success=%.1f%%",
                    i,
                    miners[i].valid_blocks,
                    miners[i].invalid_blocks,
                    miners[i].total_credits,
                    success_rate);
    }

    log_message("STATISTICS", "Total Blocks Processed: %d", total_blocks);
    log_message("STATISTICS", "Valid Blocks: %d (%.1f%%)", total_valid_blocks, total_blocks > 0 ? (total_valid_blocks * 100.0 / total_blocks) : 0.0);

    /* Blocks in ledger */
    sem_wait(&ledger->ledger_sem);
    int in_ledger = 0;
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] == '\0') {
            break;
        }
        in_ledger++;
    }
    sem_post(&ledger->ledger_sem);

    log_message("STATISTICS", "Blocks in Blockchain: %d / %d", in_ledger, shared_config->blockchain_blocks);

    /* Pending transactions */
    int pending = 0;
    sem_wait(&tx_pool->pool_sem);
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            pending++;
        }
    }
    sem_post(&tx_pool->pool_sem);
    
    log_message("STATISTICS", "Pending Transactions: %d / %d", pending, shared_config->tx_pool_size);

    /* Average transaction processing time */
    if (processed_tx_count > 0) {
        double avg_ms = (double)total_processing_us / processed_tx_count / 1000.0;
        log_message("STATISTICS", "Avg Transaction Processing Time: %.2fs", avg_ms);
    }

    log_message("STATISTICS", "==========================================");
}

/*
 * Message Processing
 * Reads incoming validation results from the message queue and
 * updates the internal statistical trackers accordingly.
 */
static void update_tx_times(const Block *block) {
    time_t now = time(NULL);
    for (int i = 0; i < block->transaction_count; i++) {
        long us = (long)difftime(now, block->transactions[i].timestamp) * 1000000L;
        total_processing_us += us;
        processed_tx_count++;
    }
}

static void process_one_message(const StatsMessage *msg) {
    total_blocks++;

    int mid = atoi(msg->miner_id);
    if (mid < 1 || mid > MAX_MINERS) {
        log_message("STATISTICS", "Invalid Miner ID: %s", msg->miner_id);
        return;
    }

    if (msg->valid) {
        miners[mid].valid_blocks++;
        miners[mid].total_credits += msg->credits;
        total_valid_blocks++;

        if (miners[mid].first_block_time == 0) {
            miners[mid].first_block_time = msg->timestamp;
        }
        miners[mid].last_block_time = msg->timestamp;

        /* Find block in ledger to compute tx processing times */
        sem_wait(&ledger->ledger_sem);
        for (int i = 0; i < shared_config->blockchain_blocks; i++) {
            if (ledger->blocks[i].curr_hash[0] == '\0') {
                break;
            }
            if (strcmp(ledger->blocks[i].block_id, msg->block_id) == 0) {
                update_tx_times(&ledger->blocks[i]);
                break;
            }
        }
        sem_post(&ledger->ledger_sem);

        log_message("STATISTICS", "Block %s from Miner %s: VALID (credits: %d)", msg->block_id, msg->miner_id, msg->credits);
    } else {
        miners[mid].invalid_blocks++;
        log_message("STATISTICS", "Block %s from Miner %s: INVALID", msg->block_id, msg->miner_id);
    }

}

/*
 * Resource Cleanup (Teardown)
 * Flushes the remaining message queue, prints the final system
 * statistics, and unmaps all shared memory segments before exiting.
 */
static void cleanup(void) {
    
    log_message("STATISTICS", "SIGUSR1 RECEIVED — printing final statistics");
    print_statistics();

    /* Drain any remaining messages (non-blocking) */
    StatsMessage msg;
    while (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1, IPC_NOWAIT) > 0) {
        process_one_message(&msg);
    }

    /* Report unfinished transactions */
    int pending = 0;
    sem_wait(&tx_pool->pool_sem);
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            pending++;
        }
    }
    sem_post(&tx_pool->pool_sem);

    log_message("STATISTICS", "Unfinished Transactions: %d", pending);

    int tx_sz_save = shared_config->tx_pool_size;
    int lg_sz_save = shared_config->blockchain_blocks;

    munmap(tx_pool, sizeof(TransactionPool) + sizeof(Transaction) * tx_sz_save);
    munmap(ledger, sizeof(BlockchainLedger) + sizeof(Block) * lg_sz_save);
    munmap(shared_config, sizeof(Config));

    close_logger();
    exit(EXIT_SUCCESS);
}

/*
 * Signal Handling
 * Intercepts SIGUSR1 for on-demand statistics printing and
 * SIGTERM/SIGINT for graceful process termination.
 */
static void handle_sigterm(int sig) {
    (void)sig;
    g_stop = 1;
}

static void handle_sigusr1(int sig) {
    (void)sig;
    g_print_stats = 1;
}

/*
 * Main Entry Point
 * Initializes IPC resources, connects to the message queue, and
 * enters the main blocking loop to process incoming statistics.
 */
int main(void) {

    struct sigaction sa_usr1 = { .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART };
    struct sigaction sa_term = { .sa_handler = handle_sigterm, .sa_flags = 0 };
    sigemptyset(&sa_usr1.sa_mask);
    sigemptyset(&sa_term.sa_mask);
    sigaction(SIGUSR1, &sa_usr1, NULL);
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    init_logger();
    log_message("STATISTICS", "Statistics process started");

    shared_config = access_shared_config();
    tx_pool = access_shared_tx_pool(shared_config->tx_pool_size);
    ledger = access_shared_ledger(shared_config->blockchain_blocks);

    key_t key = ftok(CONFIG_FILE, 'M');
    if (key == -1) {
        perror("ftok");
        exit(EXIT_FAILURE);
    }

    msgid = msgget(key, 0666);
    if (msgid == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    log_message("STATISTICS", "Connected to message queue (ID: %d)", msgid);

    StatsMessage msg;
    while (!g_stop) {
        /* Check if a SIGUSR1 arrived between iterations */
        if (g_print_stats) {
            g_print_stats = 0;
            log_message("STATISTICS", "SIGUSR1 RECEIVED");
            print_statistics();
        }

        /* Blocking receive - interrupted by signals via SA_RESTART */
        ssize_t ret = msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1, 0);
        if (ret > 0) {
            process_one_message(&msg);
        } else if (ret == -1) {
            if (errno == EINTR) {
                continue; /* woken by SIGUSR1/SIGTERM */
            }
            if (errno == EIDRM) {
                log_message("STATISTICS", "Message queue removed, exiting");
                break;
            }
            perror("msgrcv");
        }
    }

    cleanup();
    return 0;
}