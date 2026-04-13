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
#include <sys/types.h> 
#include <sys/wait.h>   
#include <signal.h>     
#include <sys/ipc.h>       
#include <fcntl.h>      
#include <sys/mman.h>   
#include <string.h>     
#include <errno.h>      
#include <sys/msg.h>    
#include <sys/stat.h>
#include <time.h>
#include "shared.h"
#include "logging.h"

/*
 * Configuration and Global State
 * Macro definitions, fixed parameters and global variables
 * shared throughout the controller process.
 */
#define CONFIG_FILE         "config.cfg"
#define SHM_CONFIG_NAME     "/deichain_config"
#define SHM_TX_POOL_NAME    "/deichain_tx_pool"
#define SHM_LEDGER_NAME     "/deichain_ledger"
#define VALIDATOR_PIPE      "VALIDATOR_PIPE"
#define SCALE_INTERVAL_S    2

static Config config;
static pid_t miner_pid = -1;
static pid_t validator_pids[3] = {-1, -1, -1};
static pid_t statistics_pid = -1;
static int msgid = -1;
static volatile sig_atomic_t g_shutdown = 0;

/*
 * Configuration Parsing and Validation
 * Helper functions to extract and validate parameters
 * from the configuration file (config.cfg).
 */
static void config_error(const char *msg) {
    fprintf(stderr, "Error in the configuration file %s\n", msg);
    exit(EXIT_FAILURE);
}

static void read_config(const char *filename) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        config_error("The file could not be opened.");
    }
    
    if (fscanf(file, "%d %d %d %d", 
        &config.num_miners, 
        &config.tx_pool_size, 
        &config.transactions_per_block, 
        &config.blockchain_blocks) != 4)
    {
        fclose(file);
        config_error("Invalid format. Expected: 4 integers.");
    }
    config.active_validators = 0;
    fclose(file);
    
    if (config.num_miners < 1)
        config_error("NUM_MINERS must be ≥ 1.");
    if (config.tx_pool_size < 1)
        config_error("TX_POOL_SIZE must be ≥ 1.");
    if (config.transactions_per_block < 1)
        config_error("TRANSACTIONS_PER_BLOCK must be ≥ 1.");
    if (config.blockchain_blocks < 1)
        config_error("BLOCKCHAIN_BLOCKS must be ≥ 1.");
    if (config.transactions_per_block > config.tx_pool_size)
        config_error("TRANSACTIONS_PER_BLOCK cannot be greater than TX_POOL_SIZE.");
}

/*
 * IPC and Shared Memory Initialization
 * Creation and formatting of shared memory segments (shm),
 * semaphores, named pipes and message queues.
 */
static void create_shared_config(void) {

    int fd = shm_open(SHM_CONFIG_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, sizeof(config)) == -1) {
        perror("ftruncate config");
        exit(EXIT_FAILURE);
    }

    shared_config = mmap(NULL, sizeof(config), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shared_config == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    memcpy(shared_config, &config, sizeof(config));
    if(sem_init(&shared_config->config_sem, 1, 1) == -1) {
        perror("sem_init config_sem");
        exit(EXIT_FAILURE);
    }
}

static void create_transaction_pool(void) {

    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;

    int fd = shm_open(SHM_TX_POOL_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        perror("ftruncate tx_pool");
        exit(EXIT_FAILURE);
    }

    tx_pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (tx_pool == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    if (sem_init(&tx_pool->pool_sem, 1, 1) != 0) {
        perror("sem_init pool");
        exit(EXIT_FAILURE);
    }

    tx_pool->current_block_id = 0;
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        tx_pool->transactions[i].empty = 1;
        tx_pool->transactions[i].age = 0;
    }
    
    log_message("CONTROLLER", "SHM_TX_POOL CREATED");
}

static void create_blockchain_ledger(void) {
    
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;

    int fd = shm_open(SHM_LEDGER_NAME, O_CREAT | O_RDWR, 0666);
    if (fd == -1) {
        perror("shm_open ledger");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(fd, (off_t)size) == -1) {
        perror("ftruncate ledger");
        exit(EXIT_FAILURE);
    }

    ledger = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ledger == MAP_FAILED) {
        perror("mmap ledger");
        exit(EXIT_FAILURE);
    }

    if (sem_init(&ledger->ledger_sem, 1, 1) != 0) {
        perror("sem_init ledger");
        exit(EXIT_FAILURE);
    }
    
    memset(ledger->blocks, 0, sizeof(Block) * shared_config->blockchain_blocks);
    log_message("CONTROLLER", "SHM_LEDGER CREATED");
}

static void init_message_queue(void) {
    key_t key = ftok(CONFIG_FILE, 'M');
    if (key == -1) {
        perror("ftok for message queue");
        exit(EXIT_FAILURE);
    }

    int msgid = msgget(key, IPC_CREAT | 0666);
    if (msgid == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    log_message("CONTROLLER", "MESSAGE QUEUE CREATED WITH ID %d", msgid);
}

static void init_named_pipe(void) {
    if (mkfifo(VALIDATOR_PIPE, 0666) == -1 && errno != EEXIST) {
        perror("mkfifo");
        log_message("CONTROLLER", "Error creating pipe %s", VALIDATOR_PIPE);
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "NAMED PIPE CREATED");
}

/*
 * Process Launchers
 * Functions responsible for forking and executing the
 * constituent processes of the simulation (Miner, Validator, Statistics).
 */
static pid_t start_miner_process(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork miner");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        execl("./miner", "miner", SHM_CONFIG_NAME, SHM_TX_POOL_NAME, SHM_LEDGER_NAME, NULL);
        perror("execl miner");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS MINER CREATED WITH PID %d", pid);
    return pid;
}

static pid_t start_validator_process(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork validator");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        execl("./validator", "validator", NULL);
        perror("execl validator");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS VALIDATOR CREATED WITH PID %d", pid);
    return pid;
}

static pid_t start_statistics_process(void) {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork statistics");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        execl("./statistics", "statistics", NULL);
        perror("execl statistics");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS STATISTICS CREATED WITH PID %d", pid);
    return pid;
}

/*
 * Dynamic Scaling (Auto-Scaling)
 * Adjusts the number of active validators based on the current
 * load of the Transaction Pool, launching or terminating processes.
 */
static void scale_validators(int new_count) {
    sem_wait(&shared_config->config_sem);
    int current = shared_config->active_validators;

    if (new_count > current) {
        for (int i = current; i < new_count; i++) {
            pid_t pid = start_validator_process();
            validator_pids[i] = pid;
        }
        log_message("CONTROLLER", "Scaled validators %d -> %d", current, new_count);
    } else if (new_count < current) {
        for (int i = current - 1; i >= new_count; i--) {
            if (validator_pids[i] > 0) {
                kill(validator_pids[i], SIGTERM);
                waitpid(validator_pids[i], NULL, 0);
                log_message("CONTROLLER", "VALIDATOR PID %d TERMINATED", validator_pids[i]);
                validator_pids[i] = -1;
            }
        }
        log_message("CONTROLLER", "Scaled validators %d -> %d", current, new_count);
    }

    shared_config->active_validators = new_count;
    sem_post(&shared_config->config_sem);
}

/*
 * Monitoring and Debug Utilities
 * Functions for calculating metrics (pool occupancy) and
 * printing a structured dump of the current blockchain state.
 */
static void dump_ledger(void) {
    if (ledger == NULL || shared_config == NULL) {
        return;
    }

    sem_wait(&ledger->ledger_sem);
    log_message("CONTROLLER", "Dumping the Ledger");

    printf("\n=================== Start Ledger ===================\n");

    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        Block *b = &ledger->blocks[i];
        if (b->curr_hash[0] == '\0') {
            break;
        }

        printf("||---- Block %03d --\n", i);
        printf("Block ID: %s\n", b->block_id);
        printf("Previous Hash:\n%s\n", b->prev_hash);
        printf("Block Timestamp: %ld\n", b->timestamp);
        printf("Nonce: %d\n", b->nonce);
        printf("Transactions:\n");
        for (int j = 0; j < b->transaction_count; j++) {
            Transaction *tx = &b->transactions[j];
            printf(" [%d] ID: %s | Reward: %d | Value: %.2f | Timestamp: %ld\n", j, tx->id, tx->reward, (float)tx->value, tx->timestamp);
        }
        printf("||------------------------------\n");
    }
    printf("=================== End Ledger ===================\n");

    sem_post(&ledger->ledger_sem);
}

static int calculate_pool_occupancy(void) {
    sem_wait(&tx_pool->pool_sem);
    
    int occupied = 0;
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            occupied++;
        }
    }
    
    sem_post(&tx_pool->pool_sem);
    return (occupied * 100) / shared_config->tx_pool_size;
}

/*
 * Resource Cleanup (Teardown)
 * Graceful termination of child processes and release of
 * all OS-level IPC resources.
 */
static void cleanup(void) {
    log_message("CONTROLLER", "WAITING FOR LAST TASKS TO FINISH");
    dump_ledger();

    if (miner_pid > 0) {
        kill(miner_pid, SIGTERM);
        waitpid(miner_pid, NULL, 0);
    }

    for (int i = 0; i < 3; i++) {
        if (validator_pids[i] > 0) {
            kill(validator_pids[i], SIGTERM);
            waitpid(validator_pids[i], NULL, 0);
        }
    }
    
    if (statistics_pid > 0) {
        kill(statistics_pid, SIGTERM);
        waitpid(statistics_pid, NULL, 0);
    }

    sem_destroy(&tx_pool->pool_sem);
    sem_destroy(&ledger->ledger_sem);
    sem_destroy(&shared_config->config_sem);
    
    size_t tx_pool_size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    size_t ledger_size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
    
    munmap(shared_config, sizeof(Config));
    munmap(tx_pool, tx_pool_size);
    munmap(ledger, ledger_size);

    shm_unlink(SHM_CONFIG_NAME);
    shm_unlink(SHM_TX_POOL_NAME);
    shm_unlink(SHM_LEDGER_NAME);

    if (msgid != 1 && msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl IPC_RMID");
    }

    unlink(VALIDATOR_PIPE);

    log_message("CONTROLLER", "CLOSING SIMULATION...");
    close_logger();
    exit(EXIT_SUCCESS);
}

/*
 * Signal Handling
 * Asynchronous routines for handling signals (SIGINT, SIGUSR1),
 * ensuring safe termination and on-demand state dumps.
 */
static void handle_sigint(int signum) {
    (void)signum;
    g_shutdown = 1;
}

static void handle_sigusr1(int signum) {
    (void)signum;
    log_message("CONTROLLER", "SIGNAL SIGUSR1 RECEIVED");
    dump_ledger();
}

/*
 * Main Entry Point
 * Overall system orchestration: bootstrap, continuous monitoring
 * loop of the transaction pool and final teardown.
 */
int main(void) {
    struct sigaction sa_int = { .sa_handler = handle_sigint, .sa_flags = SA_RESTART };
    struct sigaction sa_usr1 = { .sa_handler = handle_sigusr1, .sa_flags = SA_RESTART };
    sigemptyset(&sa_int.sa_mask);
    sigemptyset(&sa_usr1.sa_mask);
    sigaction(SIGINT, &sa_int, NULL);
    sigaction(SIGUSR1, &sa_usr1, NULL);

    init_logger();
    log_message("CONTROLLER", "DEI_CHAIN SIMULATOR STARTING");

    read_config(CONFIG_FILE);
    create_shared_config();
    init_named_pipe();
    create_transaction_pool();
    create_blockchain_ledger();
    init_message_queue();
    
    miner_pid = start_miner_process();
    validator_pids[0] = start_validator_process();
    statistics_pid = start_statistics_process();
    shared_config->active_validators = 1;

    while (!g_shutdown) {

        sleep(SCALE_INTERVAL_S);
        if (g_shutdown) {
            break;
        }

        int occupancy = calculate_pool_occupancy();

        sem_wait(&shared_config->config_sem);
        int current = shared_config->active_validators;
        sem_post(&shared_config->config_sem);
        
        if (occupancy >= 80 && current < 3) {
            scale_validators(3);
        } else if (occupancy >= 60 && current < 2) {
            scale_validators(2);
        } else if (occupancy < 40 && current > 1) {
            scale_validators(1);
        }
    }
    
    cleanup();
    return 0;
}
