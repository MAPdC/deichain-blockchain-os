/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#define _GNU_SOURCE
#include <stdio.h>      // fprintf, fscanf, fopen, perror, printf
#include <stdlib.h>     // exit, EXIT_FAILURE
#include <unistd.h>     // fork, execl, sleep
#include <sys/types.h>  // pid_t
#include <sys/wait.h>   // desnecessária
#include <signal.h>     // desnecessária
#include <sys/ipc.h>    // ftok
#include <sys/shm.h>    // desnecessária
#include <fcntl.h>      // O_CREAT, O_RDWR
#include <sys/mman.h>   // shm_open, ftruncate, mmap, MAP_SHARED, PROT_READ, PROT_WRITE
#include <string.h>     // memcpy, strcmp
#include <errno.h>      // errno, EEXIST
#include <sys/msg.h>    // msgget
#include <sys/stat.h>   // 0666
#include "shared.h"
#include "logging.h"

// MACROS
#define CONFIG_FILE "config.cfg"
#define SHM_CONFIG_NAME "/deichain_config"
#define SHM_TX_POOL_NAME "/deichain_tx_pool"
#define SHM_LEDGER_NAME "/deichain_ledger"
#define VALIDATOR_PIPE "VALIDATOR_PIPE"
#define KEY_POOL 0x1234                 // desnecessária
#define KEY_LEDGER 0x5678               // desnecessária

Config config;

pid_t miner_pid, validator_pid, statistics_pid;
int msgid;

// Função para imprimir mensagem de erro no config file
void config_error(const char *msg) {
    fprintf(stderr, "Erro no ficheiro de configuração: %s\n", msg);
    exit(EXIT_FAILURE);
}

// Lê o config file
void read_config(const char *filename) {
    FILE *file = fopen(filename, "r");

    // Verifica se foi possível abrir o ficheiro
    if (!file) {
        config_error("Não foi possível abrir o ficheiro.");
    }
    
    // Lê os parâmetros
    if (fscanf(file, "%d %d %d %d", &config.num_miners, &config.tx_pool_size, &config.transactions_per_block, &config.blockchain_blocks) != 4) {
        log_message("CONTROLLER", "Leu num_miners %d", config.num_miners);
        fclose(file);
        config_error("Formato inválido. Esperado: 4 inteiros.");
    }

    config.active_validators = 0;

    fclose(file);
    
    // Validações dos parâmetros
    if (config.num_miners < 1)
        config_error("NUM_MINERS deve ser ≥ 1.");
    if (config.tx_pool_size < 1)
        config_error("TX_POOL_SIZE deve ser ≥ 1.");
    if (config.transactions_per_block < 1)
        config_error("TRANSACTIONS_PER_BLOCK deve ser ≥ 1.");
    if (config.blockchain_blocks < 1)
        config_error("BLOCKCHAIN_BLOCKS deve ser ≥ 1.");
    if (config.transactions_per_block > config.tx_pool_size)
        config_error("TRANSACTIONS_PER_BLOCK não pode ser maior que TX_POOL_SIZE.");
}

// Adicionar config a shm
void create_shared_config() {
    size_t size = sizeof(Config);

    // Criar objeto de memória compartilhada
    int shm_fd = shm_open(SHM_CONFIG_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    // Definir o tamanho
    if (ftruncate(shm_fd, size) == -1) {
        perror("ftruncate config");
        exit(EXIT_FAILURE);
    }

    // Mapear na memória
    shared_config = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_config == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    if(sem_init(&shared_config->config_sem, 1, 1) == -1) {
        perror("sem_init config_sem");
        exit(EXIT_FAILURE);
    }

    // Copiar os dados
    memcpy(shared_config, &config, size);
    close(shm_fd);
}


// Inicializar as shared memories
TransactionPool *create_transaction_pool() {
    if (shared_config == NULL) {
        perror("shared_config não inicializado");
        exit(EXIT_FAILURE);
    }

    int tx_pool_size = shared_config->tx_pool_size;
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * tx_pool_size;

    // Criar objeto de memória compartilhada
    int shm_fd = shm_open(SHM_TX_POOL_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(shm_fd, size) == -1) {
        perror("ftruncate tx_pool");
        exit(EXIT_FAILURE);
    }

    TransactionPool *pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pool == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);

    // Inicialização do pool
    if (sem_init(&pool->pool_sem, 1, 1) != 0) {
        perror("sem_init pool");
        exit(EXIT_FAILURE);
    }

    pool->current_block_id = 0;
    for (int i = 0; i < tx_pool_size; i++) {
        pool->transactions[i].empty = 1;
        pool->transactions[i].age = 0;
    }
    
    log_message("CONTROLLER", "SHM_TX_POOL CREATED");
    return pool;
}

BlockchainLedger *create_blockchain_ledger() {
    if (shared_config == NULL) {
        perror("shared_config não inicializado");
        exit(EXIT_FAILURE);
    }

    int blockchain_blocks = shared_config->blockchain_blocks;
    size_t size = sizeof(BlockchainLedger) + sizeof(Block) * blockchain_blocks;

    // Criar objeto de memória compartilhada
    int shm_fd = shm_open(SHM_LEDGER_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open ledger");
        exit(EXIT_FAILURE);
    }

    if (ftruncate(shm_fd, size) == -1) {
        perror("ftruncate ledger");
        exit(EXIT_FAILURE);
    }

    BlockchainLedger *ledger = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ledger == MAP_FAILED) {
        perror("mmap ledger");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);

    if (sem_init(&ledger->ledger_sem, 1, 1) != 0) {
        perror("sem_init ledger");
        exit(EXIT_FAILURE);
    }
    
    log_message("CONTROLLER", "SHM_LEDGER CREATED");
    return ledger;
}

// Inicializar message queue
int init_message_queue() {
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
    return msgid;
}


// Criar NAMED PIPE
void init_named_pipe() {
    if (mkfifo(VALIDATOR_PIPE, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo");
            log_message("CONTROLLER", "Erro ao criar pipe %s", VALIDATOR_PIPE);
            exit(EXIT_FAILURE);
        }
    }
    log_message("CONTROLLER", "FIFO único criado: %s", VALIDATOR_PIPE);
}

void scale_validators(int new_count) {
    sem_wait(&shared_config->config_sem);

    if (new_count > shared_config->active_validators) {
        // Criar novos validadores
        for (int i = shared_config->active_validators; i < new_count; i++) {
            log_message("CONTROLLER", "%d", i);

            pid_t pid = fork();
            if (pid == 0) {
                execl("./validator", "validator", NULL);
                perror("execl validator");
                exit(EXIT_FAILURE);
            }
            log_message("CONTROLLER", "Validador %d criado com PID %d", i, pid);
        }
    } else if (new_count < shared_config->active_validators) {
    }

    shared_config->active_validators = new_count;
    sem_post(&shared_config->config_sem);
}


// Iniciar processo Miner com NUM_MINERS threads
pid_t start_miner_process() {
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork failed");
        exit(EXIT_FAILURE);
    }
    if (pid == 0) {
        // Passar os nomes das SHMs como argumentos
        execl("./miner", "miner", SHM_CONFIG_NAME, SHM_TX_POOL_NAME, SHM_LEDGER_NAME, NULL);
        perror("execl miner");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS MINER CREATED WITH PID %d", pid);
    return pid;
}

// Iniciar processo Validator
pid_t start_validator_process() {
    pid_t pid = fork();
    if (pid == 0) {
        execl("./validator", "validator", NULL);
        perror("execl validator");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS VALIDATOR CREATED WITH PID %d", pid);
    return pid;
}

// Iniciar processo Statistics
pid_t start_statistics_process() {
    pid_t pid = fork();
    if (pid == 0) {
        execl("./statistics", "statistics", NULL);
        perror("execl statistics");
        exit(EXIT_FAILURE);
    }
    log_message("CONTROLLER", "PROCESS STATISTICS CREATED WITH PID %d", pid);
    return pid;
}

void print_ledger() {
    if (ledger == NULL || shared_config == NULL) {
        fprintf(stderr, "Error: Ledger or config not initialized\n");
        return;
    }

    sem_wait(&ledger->ledger_sem);  // Bloquear o semáforo para acesso seguro

    sleep(3);
    printf("\n=== Blockchain Ledger ===\n");

    for (int i = 0; i < shared_config->blockchain_blocks; i++) {
        // Parar se encontrar bloco inválido (não o primeiro)
        if ((ledger->blocks[i].block_id[0] == '\0' || strcmp(ledger->blocks[i].block_id, "0") == 0) && i != 0) {
            break;
        }

        // Cabeçalho do bloco
        printf("╔══ Block %03d ", i);
        for (int j = 0; j < 50; j++) printf("═");
        printf("\n");

        printf("║ ID: BLOCK-%d-%s\n", getpid(), ledger->blocks[i].block_id);
        printf("║ Prev Hash:\n");
        printf("║ %s\n", ledger->blocks[i].prev_hash);
        printf("║ Block Timestamp: %ld\n", ledger->blocks[i].timestamp);
        printf("║ Nonce: %d\n", ledger->blocks[i].nonce);
        
        // Transações
        printf("║ Transactions (%d):\n", ledger->blocks[i].transaction_count);
        for (int j = 0; j < ledger->blocks[i].transaction_count; j++) {
            Transaction tx = ledger->blocks[i].transactions[j];
            printf("║   [%02d] ID: TX-%d-%s | Reward: %d | Value: %.2f | Timestamp: %ld\n",
                j, tx.sender_pid, tx.id, tx.reward, (float)tx.value, tx.timestamp);
        }

        // Rodapé do bloco
        printf("╚");
        for (int j = 0; j < 60; j++) printf("═");
        printf("\n\n");
    }

    sem_post(&ledger->ledger_sem);  // Liberar o semáforo
}

int calculate_pool_occupancy() {
    sem_wait(&tx_pool->pool_sem); // Bloqueia o pool para acesso seguro
    
    int occupied = 0;
    for (int i = 0; i < shared_config->tx_pool_size; i++) {
        if (!tx_pool->transactions[i].empty) {
            occupied++;
        }
    }
    
    sem_post(&tx_pool->pool_sem); // Libera o pool
    return (occupied * 100) / shared_config->tx_pool_size; // % ocupado
}

// Captura SIGINT, termina a simulação e liberta recursos
void cleanup() {

    print_ledger();

    log_message("CONTROLLER", "CLOSING SIMULATION...");

    // Terminar processos
    kill(miner_pid, SIGTERM);
    kill(validator_pid, SIGTERM);
    kill(statistics_pid, SIGTERM);

    wait(NULL);
    wait(NULL);
    wait(NULL);

    // Destruir semáforos
    sem_destroy(&tx_pool->pool_sem);
    sem_destroy(&ledger->ledger_sem);
    
    // Desmapear memórias compartilhadas
    size_t tx_pool_size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    size_t ledger_size = sizeof(BlockchainLedger) + sizeof(Block) * shared_config->blockchain_blocks;
    
    munmap(shared_config, sizeof(Config));
    munmap(tx_pool, tx_pool_size);
    munmap(ledger, ledger_size);

    // Remover objetos de memória compartilhada
    shm_unlink(SHM_CONFIG_NAME);
    shm_unlink(SHM_TX_POOL_NAME);
    shm_unlink(SHM_LEDGER_NAME);

    // Limpeza da message queue e FIFOs (mantida igual)
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl (remover fila)");
    } else {
        log_message("CONTROLLER", "Message queue removida.");
    }

    for (int i = 0; i < 3; i++) {
        char pipe_name[32];
        snprintf(pipe_name, sizeof(pipe_name), "VALIDATOR_INPUT_%d", i);
        unlink(pipe_name);
    }
    log_message("CONTROLLER", "Todos os FIFOs removidos.");
    log_message("CONTROLLER", "FIFOs removidos.");

    log_message("CONTROLLER", "Recursos libertados e simulação terminada.");
    exit(EXIT_SUCCESS);
}

// Handler para SIGINT
void handle_sigint(int signum) {
    log_message("CONTROLLER", "Sinal SIGINT (%d) recebido.", signum);
    cleanup();
}


int main() {
    signal(SIGINT, handle_sigint);  
    init_logger();
    log_message("CONTROLLER", "DEI_CHAIN SIMULATOR STARTING");

	// Ler config file
    read_config(CONFIG_FILE);
    create_shared_config();
    init_named_pipe();
    
    // Inicializar os recursos
    tx_pool = create_transaction_pool();
    ledger = create_blockchain_ledger();
    msgid = init_message_queue();
    
	// Iniciar os processos
    miner_pid = start_miner_process();
    validator_pid = start_validator_process();
    statistics_pid = start_statistics_process();

    while (1) {

        int occupancy = calculate_pool_occupancy();
        
        if (occupancy >= 80 && shared_config->active_validators < 3) {
            scale_validators(3);
        } else if (occupancy >= 60 && shared_config->active_validators < 2) {
            scale_validators(2);
        } else if (occupancy < 40 && shared_config->active_validators > 1) {
            scale_validators(1);
        }
    }
    
    sleep(2); // Verificar a cada 2 segundos

    // Espera pelos processos filhos (bloqueia até que sejam terminados manualmente com SIGINT)
    wait(NULL);
    wait(NULL);
    wait(NULL);

    cleanup();  // Em caso de terminação normal (não por sinal)
    
    return 0;
}
