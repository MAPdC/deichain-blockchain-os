/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <openssl/sha.h>
#include <sys/msg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/mman.h>
#include <errno.h>
#include "shared.h"
#include "logging.h"

#define CONFIG_FILE "config.cfg"
#define VALIDATOR_PIPE "VALIDATOR_PIPE"

static int pipe_fd = -1;
static int msgid = -1;

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

// Função auxiliar para encontrar o último bloco válido
int find_last_valid_block() {
    int last_valid = -1; // -1 indica blockchain vazia
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] == '\0') { // Bloco não inicializado
            break;
        }
        last_valid = i;
    }
    return last_valid;
}

// Verifica se o Proof of Work é válido
int validate_pow(const Block *block) {
    char calculated_hash[SHA256_DIGEST_LENGTH*2+1];
    calculate_block_hash(block, calculated_hash);
    
    // Verificação em 2 passos:
    // 1. Hash calculado bate com o declarado
    if (strcmp(calculated_hash, block->curr_hash) != 0) {
        log_message("VALIDATOR", "Discrepância de hash: Calculado=%s vs Declarado=%s",
                   calculated_hash, block->curr_hash);
        return 0;
    }
    
    // 2. Dificuldade satisfeita
    for (int i = 0; i < block->difficulty; i++) {
        if (block->curr_hash[i] != '0') return 0;
    }
    
    return 1;
}

// Valida todas as transações de um bloco
int validate_transactions(Block *block) {
    sem_wait(&tx_pool->pool_sem);
    int valid = 1;
    for (int i = 0; i < block->transaction_count; i++) {
        int found = 0;
        for (int j = 0; j < shared_config->tx_pool_size; j++) {
            if (strcmp(tx_pool->transactions[j].id, block->transactions[i].id) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) {
            valid = 0;
            break;
        }
    }
    sem_post(&tx_pool->pool_sem);
    return valid;
}

// Função principal de validação de blocos
int validate_block(Block *block) {
    // 1. Verificar Proof of Work
    if (!validate_pow(block)) {
        log_message("VALIDATOR", "Bloco %s: Proof of Work inválido", block->block_id);
        return 0;
    }

    // 2. Verificar hash anterior
    sem_wait(&ledger->ledger_sem);
    int last_valid_idx = find_last_valid_block();
    
    if (last_valid_idx == -1) { // Blockchain vazia (bloco gênese)
        if (strcmp(block->prev_hash, "INITIAL_HASH") != 0) {
            sem_post(&ledger->ledger_sem);
            log_message("VALIDATOR", "Bloco gênese com hash anterior inválido");
            return 0;
        }
    } else {
        if (strcmp(block->prev_hash, ledger->blocks[last_valid_idx].curr_hash) != 0) {
            sem_post(&ledger->ledger_sem);
            log_message("VALIDATOR", "Hash anterior não corresponde ao último bloco válido");
            return 0;
        }
    }
    sem_post(&ledger->ledger_sem);

    // 3. Verificar transações
    if (!validate_transactions(block)) {
        log_message("VALIDATOR", "Transações inválidas no bloco %s", block->block_id);
        return 0;
    }

    return 1;
}

// Adiciona um bloco válido ao ledger
void add_to_ledger(Block *block) {
    sem_wait(&ledger->ledger_sem);
    
    int next_idx = find_last_valid_block() + 1;
    if (next_idx >= shared_config->blockchain_blocks) {
        sem_post(&ledger->ledger_sem);
        log_message("VALIDATOR", "Ledger cheio - não é possível adicionar mais blocos");
        return;
    }

    ledger->blocks[next_idx] = *block;
    sem_post(&ledger->ledger_sem);
    
    log_message("VALIDATOR", "Bloco %s adicionado ao ledger na posição %d\n\n", 
               block->block_id, next_idx);
}

void send_block_statistics(const Block *block, int valid) {
    if (!block || !block->block_id[0]) {
        log_message("VALIDATOR", "Invalid block data for statistics");
        return;
    }

    StatsMessage msg = {
        .mtype = 1,
        .valid = valid,
        .timestamp = time(NULL)
    };
    
    // Converter miner_id para string
    snprintf(msg.miner_id, sizeof(msg.miner_id), "%d", block->miner_id);
    
    // Copiar block_id
    strncpy(msg.block_id, block->block_id, sizeof(msg.block_id) - 1);
    msg.block_id[sizeof(msg.block_id) - 1] = '\0';
    
    // Calcular créditos (soma das recompensas)
    msg.credits = 0;
    for (int i = 0; i < block->transaction_count; i++) {
        msg.credits += block->transactions[i].reward;
    }
    
    if (msgsnd(msgid, &msg, sizeof(StatsMessage) - sizeof(long), 0) == -1) {
        perror("msgsnd");
        log_message("VALIDATOR", "Failed to send stats for block %s", block->block_id);
    }
}

// Processa um bloco recebido
void process_block(Block *block) {
    int valid = validate_block(block);

    if (valid) {
        sem_wait(&tx_pool->pool_sem);
        for (int i = 0; i < block->transaction_count; i++) {
            for (int j = 0; j < shared_config->tx_pool_size; j++) {
                if (!tx_pool->transactions[j].empty && 
                    strcmp(tx_pool->transactions[j].id, block->transactions[i].id) == 0) {
                    tx_pool->transactions[j].empty = 1; // Marcar como processada
                    break;
                }
            }
        }
        sem_post(&tx_pool->pool_sem);
        add_to_ledger(block);
    }
    send_block_statistics(block, valid);
}

void age_transactions() {
    sem_wait(&tx_pool->pool_sem);
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            tx_pool->transactions[i].age++;
            if (tx_pool->transactions[i].age % 50 == 0) {
                tx_pool->transactions[i].reward++;
            }
        }
    }
    sem_post(&tx_pool->pool_sem);
}

// Liberta recursos
void cleanup() {
    log_message("VALIDATOR", "Liberando recursos...");
    
    // Fechar pipe
    if (pipe_fd != -1) {
        close(pipe_fd);
    }
    
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
    
    // Fechar logger
    close_logger();
    
    exit(EXIT_SUCCESS);
}

// Handler para SIGINT
void handle_sigint(int sig) {
    (void)sig;
    log_message("VALIDATOR", "Sinal SIGINT recebido");
    cleanup();
}

int main() {
    signal(SIGTERM, handle_sigint);
    signal(SIGINT, handle_sigint);
    init_logger();

    // Inicializar memórias compartilhadas
    shared_config = access_shared_config("/deichain_config");
    tx_pool = access_shared_tx_pool("/deichain_tx_pool", shared_config->tx_pool_size);
    ledger = access_shared_ledger("/deichain_ledger", shared_config->blockchain_blocks);

    log_message("VALIDATOR", "Validator iniciado e pronto para processar blocos");
    
    pipe_fd = open(VALIDATOR_PIPE, O_RDONLY);
    if (pipe_fd == -1) {
        log_message("VALIDATOR", "Erro ao abrir pipe");
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

    // Loop principal
    while (1) {
        age_transactions();
        Block block;
        ssize_t bytes_read = read(pipe_fd, &block, sizeof(Block));
        
        if (bytes_read == sizeof(Block)) {
            process_block(&block);

        } else if (bytes_read == -1 && errno != EINTR) {
            perror("read");
            break;
        }
        sleep(1);
    }

    cleanup();
    return 0;
}
