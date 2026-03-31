/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

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
#include "shared.h"
#include "logging.h"

#define VALIDATOR_PIPE "VALIDATOR_PIPE"
int pipe_fd = -1;

// Função para mapear a memória compartilhada de configuração
Config* access_shared_config(const char* shm_name) {
    size_t size = sizeof(Config);
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    shared_config = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_config == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);
    return shared_config;
}

// Função para mapear o pool de transações
TransactionPool* access_shared_tx_pool(const char* shm_name) {
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    tx_pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (tx_pool == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);
    return tx_pool;
}

// Função para mapear o ledger da blockchain
BlockchainLedger* access_shared_ledger(const char* shm_name) {
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open ledger");
        exit(EXIT_FAILURE);
    }

    ledger = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ledger == MAP_FAILED) {
        perror("mmap ledger");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);
    return ledger;
}

int solve_pow(Block *block, int difficulty) {
    block->nonce = 0;
    block->difficulty = difficulty;
    
    while (1) {

        calculate_block_hash((const Block*)block, block->curr_hash);
        
        // Verificar dificuldade
        int valid = 1;
        for (int i = 0; i < difficulty; i++) {
            if (block->curr_hash[i] != '0') {
                valid = 0;
                break;
            }
        }
        
        if (valid) return block->nonce;
        block->nonce++;
    }
}

int compare_transactions(const void *a, const void *b) {
    Transaction *tx1 = (Transaction *)a;
    Transaction *tx2 = (Transaction *)b;
    return tx2->reward - tx1->reward;
}

int calculate_difficulty(int total_reward, int base_difficulty) {
    // Garantir um mínimo e máximo de dificuldade
    int adjusted = base_difficulty + (total_reward / 100);
    return (adjusted < 2) ? 2 : (adjusted > 8) ? 8 : adjusted;
}

int is_ledger_empty(BlockchainLedger *ledger) {
    sem_wait(&ledger->ledger_sem); // Bloqueia o acesso concorrente
    
    int empty = 1;
    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] != '\0') {
            empty = 0;
            break;
        }
    }
    
    sem_post(&ledger->ledger_sem); // Libera o semáforo
    return empty;
}

int find_last_valid_block(BlockchainLedger *ledger, Config *config) {
    sem_wait(&ledger->ledger_sem); // Bloqueia o acesso concorrente

    int last_valid_idx = -1; // Retorna -1 se a ledger estiver vazia
    for (int i = 0; i < config->blockchain_blocks; i++) {
        if (ledger->blocks[i].curr_hash[0] != '\0') {
            last_valid_idx = i;
        } else {
            break;
        }
    }

    sem_post(&ledger->ledger_sem); // Libera o semáforo
    return last_valid_idx;
}

void open_validator_pipe() {
    // Criar o named pipe se não existir
    if (access(VALIDATOR_PIPE, F_OK) == -1) {
        if (mkfifo(VALIDATOR_PIPE, 0666) == -1) {
            log_message("MINER", "Erro ao criar o pipe %s: %s", VALIDATOR_PIPE, strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    // Abrir para escrita
    pipe_fd = open(VALIDATOR_PIPE, O_WRONLY);
    if (pipe_fd == -1) {
        log_message("MINER", "Erro ao abrir o pipe %s: %s", VALIDATOR_PIPE, strerror(errno));
        exit(EXIT_FAILURE);
    } else {
        log_message("MINER", "Pipe %s aberto com sucesso", VALIDATOR_PIPE);
    }
}

void close_pipe() {
    if (pipe_fd != -1) {
        close(pipe_fd);
        log_message("MINER", "Pipe fechado");
        pipe_fd = -1;
    }
}

void send_block_to_validator(Block *block) {
    if (pipe_fd == -1) {
        log_message("MINER", "Pipe não está disponível para escrita");
        return;
    }

    ssize_t bytes_written = write(pipe_fd, block, sizeof(Block));
    if (bytes_written == -1) {
        log_message("MINER", "Erro ao escrever no pipe: %s", strerror(errno));
    }
}

void* miner_thread(void* arg) {
    MinerData* data = (MinerData*)arg;
    log_message("MINER", "Thread do minerador %d iniciada", data->miner_id);

    int counter = 0;
    while (1) {
        Block new_block;
        
        counter++;
        snprintf(new_block.block_id, sizeof(new_block.block_id), "Miner-%d#%d", 
                data->miner_id, counter + 1);
        
        // Configurar hash anterior
        if (is_ledger_empty(ledger) == 1) {
            strcpy(new_block.prev_hash, "INITIAL_HASH");
        } else {
            int last_valid_idx = find_last_valid_block(ledger, shared_config);
            strcpy(new_block.prev_hash, ledger->blocks[last_valid_idx].curr_hash);
        }

        new_block.miner_id = data->miner_id;
        new_block.timestamp = time(NULL);
        new_block.transaction_count = 0;

        // Selecionar transações com melhor recompensa
        sem_wait(&tx_pool->pool_sem);
        
        // Criar cópia local para ordenação
        Transaction* tx_copy = malloc(shared_config->tx_pool_size * sizeof(Transaction));
        memcpy(tx_copy, tx_pool->transactions, shared_config->tx_pool_size * sizeof(Transaction));
        
        // Ordenar por recompensa (decrescente)
        qsort(tx_copy, shared_config->tx_pool_size, sizeof(Transaction), compare_transactions);
        
        int total_reward = 0;
        // Selecionar as melhores transações disponíveis
        for (int i = 0; i < shared_config->tx_pool_size && 
             new_block.transaction_count < shared_config->transactions_per_block; i++) {
            if (!tx_copy[i].empty) {
                new_block.transactions[new_block.transaction_count] = tx_copy[i];
                total_reward += tx_pool->transactions[i].reward;
                new_block.transaction_count++;
            }
        }
        
        free(tx_copy);
        sem_post(&tx_pool->pool_sem);

        if (new_block.transaction_count == 0) {
            sleep(5);
            continue;
        }

        // Realizar Proof of Work
        int base_diff = 3;
        int cur_diff = calculate_difficulty(total_reward, base_diff);
        new_block.difficulty = cur_diff;
        solve_pow(&new_block, cur_diff);

        // Enviar bloco ao validator
        send_block_to_validator(&new_block);

        // Pausar antes de minerar o próximo bloco
        sleep(1);
    }

    return NULL;
}


int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <config_shm> <tx_pool_shm> <ledger_shm>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    init_logger();

    // Mapear as memórias compartilhadas
    shared_config = access_shared_config(argv[1]);
    tx_pool = access_shared_tx_pool(argv[2]);
    ledger = access_shared_ledger(argv[3]);

    open_validator_pipe();

    log_message("MINER", "Processo miner iniciado com %d threads.", shared_config->num_miners);

    pthread_t threads[shared_config->num_miners];
    MinerData thread_data[shared_config->num_miners];

    // Criar threads dos mineradores
    for (int i = 0; i < shared_config->num_miners; i++) {
        thread_data[i].miner_id = i + 1;
        
        if (pthread_create(&threads[i], NULL, miner_thread, &thread_data[i])) {
            log_message("MINER", "Erro ao criar a thread do minerador: %s", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    log_message("MINER", "Todos os mineradores iniciados, a aguardar trabalho.");
    
    // Esperar pelas threads
    for (int i = 0; i < shared_config->num_miners; i++) {
        pthread_join(threads[i], NULL);
    }

    //UNMAP
    if (shared_config != NULL) {
        munmap(shared_config, sizeof(Config));
    }

    if (tx_pool != NULL) {
        size_t tx_pool_size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
        munmap(tx_pool, tx_pool_size);
    }

    if (ledger != NULL) {
        size_t ledger_size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
        munmap(ledger, ledger_size);
    }

    close_pipe();
    close_logger();
    return 0;
}
