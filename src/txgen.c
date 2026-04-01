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
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include "shared.h"
#include "logging.h"

#define SHM_CONFIG_NAME "/deichain_config"
#define SHM_TX_POOL_NAME "/deichain_tx_pool"
#define MIN_REWARD 1
#define MAX_REWARD 3
#define MIN_SLEEP_MS 200
#define MAX_SLEEP_MS 3000

static volatile sig_atomic_t g_stop = 0;

/*
 * Shared Memory Accessors
 * Functions to map and attach the configuration and
 * transaction pool shared memory segments.
 */
static Config *map_shared_config(void) {
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
static TransactionPool *map_shared_tx_pool(void) {

    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    
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

/*
 * Transaction Generation
 * Populates a new Transaction structure with randomized
 * values, the specified reward, and a unique identifier.
 */
static Transaction generate_transaction(int reward, int counter) {
    Transaction tx;
    memset(&tx, 0, sizeof(tx));

    snprintf(tx.id, sizeof(tx.id), "TX-%d-%d", getpid(), counter);
    tx.reward = reward;
    tx.sender_pid = getpid();
    tx.receiver_id = rand() % 1000;
    tx.value = (rand() % 10000) + 1; /* stored as integer cents */
    tx.timestamp = time(NULL);
    tx.age = 0;
    tx.empty = 0;

    return tx;
}

/*
 * Signal Handling
 * Gracefully intercepts termination signals (SIGINT, SIGTERM)
 * to halt the generation loop and perform clean teardown.
 */
static void handle_sigint(int sig) {
    (void)sig;
    g_stop = 1;
}

/*
 * Main Entry Point
 * Parses command-line arguments, enforces boundaries, maps shared
 * resources, and executes the periodic transaction generation loop.
 */
int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <reward> <intervalo_ms>\n", argv[0]);
        fprintf(stderr, "  reward:       %d a %d\n", MIN_REWARD, MAX_REWARD);
        fprintf(stderr, "  intervalo_ms: %d a %d\n", MIN_SLEEP_MS, MAX_SLEEP_MS);
        exit(EXIT_FAILURE);
    }

    int reward = atoi(argv[1]);
    int sleep_ms = atoi(argv[2]);

    if (reward < MIN_REWARD || reward > MAX_REWARD) {
        fprintf(stderr, "ErroR: reward must be between %d and %d\n", MIN_REWARD, MAX_REWARD);
        exit(EXIT_FAILURE);
    }
    if (sleep_ms < MIN_SLEEP_MS || sleep_ms > MAX_SLEEP_MS) {
        fprintf(stderr, "Error: sleep_ms must be between %d and %d\n", MIN_SLEEP_MS, MAX_SLEEP_MS);
        exit(EXIT_FAILURE);
    }

    struct sigaction sa = { .sa_handler = handle_sigint, .sa_flags = 0 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    init_logger();
    srand((unsigned int)(time(NULL) ^ getpid()));

    shared_config = map_shared_config();
    tx_pool = map_shared_tx_pool();

    log_message("TXGEN", "Started (PID=%d, reward=%d, interval=%dms)", getpid(), reward, sleep_ms);

    int counter = 0;
    while (!g_stop) {
        sem_wait(&tx_pool->pool_sem);

        int inserted = 0;
        /* Sequential scan for first free slot, as per spec */
        for (int i = 0; i < shared_config->tx_pool_size; i++) {
            if (tx_pool->transactions[i].empty) {
                tx_pool->transactions[i] = generate_transaction(reward, counter);
                inserted = 1;
                break;
            }
        }

        sem_post(&tx_pool->pool_sem);

        if (inserted) {
            log_message("TXGEN", "TX-%d-%d inserted (reward=%d)", getpid(), counter, reward);
            counter++;
        } else {
            log_message("TXGEN", "Full pool - transaction discarded");
        }

        usleep((useconds_t)sleep_ms * 1000);
    }
    
    log_message("TXGEN", "Terminating. Total of transactions generated: %d", counter);

    size_t tx_sz = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    munmap(tx_pool, tx_sz);
    munmap(shared_config, sizeof(Config));

    close_logger();
    return 0;
}