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
#include <sys/stat.h>
#include <openssl/sha.h>
#include <string.h>
#include <errno.h>
#include "shared.h"
#include "logging.h"


#define MIN_SLEEP 200
#define MAX_SLEEP 3000

// Função para mapear a memória compartilhada de configuração
Config* map_shared_config(const char* shm_name) {
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open config");
        exit(EXIT_FAILURE);
    }

    Config *config = mmap(NULL, sizeof(Config), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (config == MAP_FAILED) {
        perror("mmap config");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);
    return config;
}

// Função para mapear o pool de transações
TransactionPool* map_shared_tx_pool(const char* shm_name) {
    // Primeiro precisamos do tamanho, que está na config
    size_t size = sizeof(TransactionPool) + sizeof(Transaction) * shared_config->tx_pool_size;
    
    int shm_fd = shm_open(shm_name, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open tx_pool");
        exit(EXIT_FAILURE);
    }

    TransactionPool *pool = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pool == MAP_FAILED) {
        perror("mmap tx_pool");
        exit(EXIT_FAILURE);
    }

    close(shm_fd);
    return pool;
}

Transaction generate_transaction(int reward, int counter) {
    Transaction new_tx = {0};  // limpa toda a estrutura

    snprintf(new_tx.id, sizeof(new_tx.id), "TX-%d-%d", getpid(), counter);
    new_tx.reward = reward;
    new_tx.sender_pid = getpid();
    new_tx.receiver_id = rand() % 1000;
    new_tx.value = (rand() % 100) + 1;
    new_tx.timestamp = time(NULL);
    new_tx.age = 0;
    new_tx.empty = 0;

    printf("TxGen: Created transaction %s\n", new_tx.id);
    return new_tx;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "%s <recompensa> <tempo de espera>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    shared_config = map_shared_config("/deichain_config");
    tx_pool = map_shared_tx_pool("/deichain_tx_pool");

    if (shared_config == NULL || tx_pool == NULL) {
        fprintf(stderr, "Erro ao mapear memórias compartilhadas.\n");
        exit(EXIT_FAILURE);
    }

    int reward = atoi(argv[1]);
    int sleep_time = atoi(argv[2]);

    if (reward < 1 || reward > 3 || sleep_time < MIN_SLEEP || sleep_time > MAX_SLEEP) {
        fprintf(stderr, "Recompensa 200-3000 || Tempo de espera 200 a 3000 ms");
        exit(EXIT_FAILURE);
    }

    int counter = 0;
    while (1) {
        sem_wait(&tx_pool->pool_sem);

        int inserted = 0;
        for (int i = 0; i < shared_config->tx_pool_size; i++) {
            if (tx_pool->transactions[i].empty) {
                tx_pool->transactions[i] = generate_transaction(reward, counter);
                inserted = 1;
                break;
            }
        }

        if (inserted) {
            counter++;
        }

        sem_post(&tx_pool->pool_sem);
        sleep(sleep_time / 1000);
    }
    
    return 0;
}
