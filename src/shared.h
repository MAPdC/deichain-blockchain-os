/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#ifndef SHARED_STRUCTS_H
#define SHARED_STRUCTS_H

#include <time.h>
#include <semaphore.h>
#include <sys/types.h>
#include <openssl/sha.h>

#define MAX_TRANSACTIONS_PER_BLOCK 10 // este valor virá do config
#define MAX_HASH_SIZE SHA256_DIGEST_LENGTH*2+1

typedef struct {
    int num_miners;
    int tx_pool_size;
    int transactions_per_block;
    int blockchain_blocks;
    int active_validators;
    sem_t config_sem;
} Config;

typedef struct {
    char id[64];
    int reward;
    int sender_pid;
    int receiver_id;
    int value;
    time_t timestamp;
    int age;
    int empty; // 1 = livre, 0 = ocupada
} Transaction;


typedef struct {
    sem_t pool_sem;                     // protege toda a Transaction Pool
    int current_block_id;
    Transaction transactions[];   // alocado dinamicamente com base em TX_POOL_SIZE
} TransactionPool;


typedef struct {
    char block_id[64];
    int miner_id;
    int transaction_count;
    time_t timestamp;
    Transaction transactions[MAX_TRANSACTIONS_PER_BLOCK];
    int nonce;
    int difficulty;
    char prev_hash[MAX_HASH_SIZE];
    char curr_hash[MAX_HASH_SIZE];
} Block;


typedef struct {
    sem_t ledger_sem;         // protege toda a Blockchain Ledger
    Block blocks[];    // alocado dinamicamente com base em BLOCKCHAIN_BLOCKS
} BlockchainLedger;

// Estrutura para mensagens da fila
typedef struct {
    long mtype; // Tipo de mensagem (1 para estatísticas)
    char miner_id[64];
    char block_id[64];
    int valid; // 1 para válido, 0 para inválido
    int credits; // Créditos ganhos (soma das recompensas)
    time_t timestamp;
} StatsMessage;

typedef struct {
    int miner_id;
} MinerData;

// Estrutura para armazenar estatísticas
typedef struct {
    int valid_blocks;
    int invalid_blocks;
    int total_credits;
    time_t first_block_time;
    time_t last_block_time;
} MinerStats;

extern Config *shared_config;
extern TransactionPool *tx_pool;
extern BlockchainLedger *ledger;

extern void calculate_block_hash(const Block *block, char output[SHA256_DIGEST_LENGTH*2+1]);

#endif
