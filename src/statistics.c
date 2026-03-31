/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

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
#include <sys/stat.h>
#include "shared.h"
#include "logging.h"

#define CONFIG_FILE "config.cfg"
#define SHM_CONFIG_NAME "/deichain_config"
#define SHM_TX_POOL_NAME "/deichain_tx_pool"
#define SHM_LEDGER_NAME "/deichain_ledger"

static MinerStats miners_stats[50] = {0};
static int total_blocks = 0;
static int total_valid_blocks = 0;
static int msgid;
static int total_processing_time = 0;
static int processed_transactions = 0;

// Função para mapear a memória compartilhada de configuração
Config* access_shared_config(const char* shm_name) {
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open config");
        return NULL;
    }

    Config *config = mmap(NULL, sizeof(Config), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (config == MAP_FAILED) {
        perror("mmap config");
        return NULL;
    }

    return config;
}

// Função para mapear o pool de transações
TransactionPool* access_shared_tx_pool(const char* shm_name, int tx_pool_size) {
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * tx_pool_size;
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open tx_pool");
        return NULL;
    }

    TransactionPool *pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (pool == MAP_FAILED) {
        perror("mmap tx_pool");
        return NULL;
    }

    return pool;
}

// Função para mapear o ledger da blockchain
BlockchainLedger* access_shared_ledger(const char* shm_name, int blockchain_blocks) {
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * blockchain_blocks;
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open ledger");
        return NULL;
    }

    BlockchainLedger *ledger = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    close(shm_fd);
    
    if (ledger == MAP_FAILED) {
        perror("mmap ledger");
        return NULL;
    }

    return ledger;
}

void print_statistics() {
    time_t now;
    time(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));

    log_message("STATISTICS", "===== Current Statistics [%s] =====", time_str);

    // Estatísticas por minerador
    for (int i = 1; i <= shared_config->num_miners; i++) {
        if (miners_stats[i].valid_blocks > 0 || miners_stats[i].invalid_blocks > 0) {
            double avg_time = 0;
            if (miners_stats[i].valid_blocks > 0) {
                avg_time = (double)(miners_stats[i].last_block_time - miners_stats[i].first_block_time) / 
                           miners_stats[i].valid_blocks;
            }

            log_message("STATISTICS",
                       "Miner %d: Valid=%d, Invalid=%d, Credits=%d, Avg Time=%.2fs, Success Rate=%.1f%%",
                       i,
                       miners_stats[i].valid_blocks,
                       miners_stats[i].invalid_blocks,
                       miners_stats[i].total_credits,
                       avg_time,
                       (miners_stats[i].valid_blocks + miners_stats[i].invalid_blocks) > 0 ?
                       (miners_stats[i].valid_blocks * 100.0 / 
                       (miners_stats[i].valid_blocks + miners_stats[i].invalid_blocks)) : 0);
        }
    }

    // Estatísticas gerais
    log_message("STATISTICS", "Total Blocks Processed: %d", total_blocks);
    log_message("STATISTICS", "Valid Blocks: %d (%.2f%%)",
                total_valid_blocks,
                total_blocks > 0 ? (total_valid_blocks * 100.0 / total_blocks) : 0);

    // Blocos no ledger
    sem_wait(&ledger->ledger_sem);
    int ledger_blocks = 0;
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].block_id[0] != '\0') {
            ledger_blocks++;
        } else {
            break;
        }
    }
    sem_post(&ledger->ledger_sem);

    log_message("STATISTICS", "Blocks in Blockchain: %d", ledger_blocks);

    // Transações pendentes
    int pending_transactions = 0;
    sem_wait(&tx_pool->pool_sem);
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) pending_transactions++;
    }
    sem_post(&tx_pool->pool_sem);
    
    log_message("STATISTICS", "Pending Transactions: %d/%d (%.1f%%)", 
                pending_transactions, shared_config->tx_pool_size,
                (pending_transactions * 100.0) / shared_config->tx_pool_size);

    // Tempo médio de processamento
    if (processed_transactions > 0) {
        double avg_processing_time = (double)total_processing_time / processed_transactions;
        log_message("STATISTICS", "Avg Transaction Processing Time: %.2fs", avg_processing_time);
    }

    log_message("STATISTICS", "==========================================");
}

void update_transaction_times(const Block *block) {
    time_t now = time(NULL);
    for (int i = 0; i < block->transaction_count; i++) {
        double processing_time = difftime(now, block->transactions[i].timestamp);
        total_processing_time += processing_time;
        processed_transactions++;
    }
}

void process_messages() {
    StatsMessage msg;

    while (1) {
        if (msgrcv(msgid, &msg, sizeof(msg) - sizeof(long), 1, 0) == -1) {
            if (errno == EINTR) {
                // Interrompido por sinal como SIGUSR1 — recomeça
                continue;
            } else if (errno == EIDRM) {
                log_message("STATISTICS", "Message queue was removed. Exiting...");
                break;
            } else {
                perror("msgrcv");
                continue;
            }
        }

        total_blocks++;

        // Converter miner_id de string para número (se necessário)
        int miner_num = atoi(msg.miner_id);
        
        // Verificar se o minerador é válido (assumindo num_miners é o número máximo)
        if (miner_num >= 1 && miner_num <= shared_config->num_miners) {
            if (msg.valid) {
                miners_stats[miner_num].valid_blocks++;
                miners_stats[miner_num].total_credits += msg.credits;
                total_valid_blocks++;

                if (miners_stats[miner_num].first_block_time == 0) {
                    miners_stats[miner_num].first_block_time = msg.timestamp;
                }
                miners_stats[miner_num].last_block_time = msg.timestamp;
                
                // Encontrar o bloco correspondente no ledger
                sem_wait(&ledger->ledger_sem);
                Block *found_block = NULL;
                for (int i = 0; i < shared_config->blockchain_blocks; i++) {
                    if (strlen(ledger->blocks[i].block_id) > 0 && 
                        strcmp(ledger->blocks[i].block_id, msg.block_id) == 0) {
                        found_block = &ledger->blocks[i];
                        break;
                    }
                }
                sem_post(&ledger->ledger_sem);

                if (found_block) {
                    update_transaction_times(found_block);
                } else {
                    log_message("STATISTICS", "Block %s not found in ledger", msg.block_id);
                }
            } else {
                miners_stats[miner_num].invalid_blocks++;
            }

            if (msg.valid) {
                log_message("STATISTICS",
                            "Block %s from Miner %s: VALID (credits: %d)",
                            msg.block_id,
                            msg.miner_id,
                            msg.credits);
            }
        } else {
            log_message("STATISTICS", "Received message from unknown miner: %s", msg.miner_id);
        }
    }
}

void cleanup() {
    log_message("STATISTICS", "Cleaning up resources...");

    // Imprime estatísticas finais
    print_statistics();
    sleep(1);

    // Desmapear memórias compartilhadas
    if (shared_config) {
        munmap(shared_config, sizeof(Config));
    }
    if (tx_pool) {
        size_t tx_pool_size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
        munmap(tx_pool, tx_pool_size);
    }
    if (ledger) {
        size_t ledger_size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
        munmap(ledger, ledger_size);
    }

    close_logger();
    exit(EXIT_SUCCESS);
}

void handle_sigterm(int sig) {
    (void)sig;
    log_message("STATISTICS", "SIGTERM received - terminating");
    cleanup();
}


void handle_sigusr1(int sig) {
    (void)sig;
    log_message("STATISTICS", "SIGUSR1 received - displaying statistics");
    print_statistics();
}

int main() {
    signal(SIGUSR1, handle_sigusr1);
    signal(SIGTERM, handle_sigterm);
    signal(SIGINT, handle_sigterm);

    init_logger();
    log_message("STATISTICS", "Statistics process started");

    // Acessar memórias compartilhadas
    shared_config = access_shared_config(SHM_CONFIG_NAME);
    if (!shared_config) {
        log_message("STATISTICS", "Failed to access shared config");
        exit(EXIT_FAILURE);
    }

    tx_pool = access_shared_tx_pool(SHM_TX_POOL_NAME, shared_config->tx_pool_size);
    if (!tx_pool) {
        log_message("STATISTICS", "Failed to access transaction pool");
        exit(EXIT_FAILURE);
    }

    ledger = access_shared_ledger(SHM_LEDGER_NAME, shared_config->blockchain_blocks);
    if (!ledger) {
        log_message("STATISTICS", "Failed to access blockchain ledger");
        exit(EXIT_FAILURE);
    }

    // Obter message queue
    key_t key = ftok(CONFIG_FILE, 'M');
    if (key == -1) {
        perror("ftok");
        log_message("STATISTICS", "Failed to create key for message queue");
        exit(EXIT_FAILURE);
    }

    msgid = msgget(key, 0666);
    if (msgid == -1) {
        perror("msgget");
        log_message("STATISTICS", "Failed to access message queue");
        exit(EXIT_FAILURE);
    }

    log_message("STATISTICS", "Connected to message queue (ID: %d)", msgid);

    // Processar mensagens
    process_messages();

    cleanup();
    return 0;
}