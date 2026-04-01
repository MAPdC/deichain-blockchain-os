# ⛓️ DEIChain — Blockchain Simulation in C

[![Language](https://img.shields.io/badge/Language-C11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![Platform](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)]()
[![Concurrency](https://img.shields.io/badge/Concurrency-POSIX_Threads_%7C_IPC-orange.svg)]()

> **Academic project** developed for the **Operating Systems** course at the **University of Coimbra** (BSc in Informatics Engineering, 2024/2025).

A concurrent, multi-process blockchain simulator written in C. This project explores **IPC** (Inter-Process Communication), **shared memory**, **POSIX semaphores**, **named pipes**, **message queues**, and **POSIX threads**.

---

## 📖 Overview

DEIChain simulates the core lifecycle of a blockchain — transaction creation, proof-of-work mining, and block validation — using real OS-level concurrency primitives. It is intentionally built without any high-level frameworks, relying entirely on Linux system calls to demonstrate how distributed systems manage shared state under concurrency.

```text
  [Transaction Generator(s)]
             │
             ▼  (Shared Memory)
      [Transaction Pool] ──────────────────────────────┐
             ▲                                         │
             │                                         ▼
       [Validator(s)] ◄──── (Named Pipe) ──── [Miner (N threads)]
             │
             ├──► [Blockchain Ledger] (Shared Memory)
             │
             └──► [Statistics] (Message Queue) ◄── SIGUSR1
```

---

## 🏗️ Architecture & Concurrency Design

| Component | Type | Role |
| --------- | ---- | ---- |
| ⚙️ **Controller** | Process | Bootstraps the system, monitors pool occupancy, and dynamically scales Validators. |
| ⛏️ **Miner** | Process + *N* threads | Each thread selects transactions, solves PoW, and sends blocks via named pipe. |
| 🛡️ **Validator** | Process (1–3) | Validates PoW, chain continuity, and transaction availability, then updates the ledger. |
| 📊 **Statistics** | Process | Collects metrics via message queue and prints them on `SIGUSR1` or termination. | 
| 💸 **TxGen** | Process | User-launched process that generates transactions and writes them to the pool. |


🔌 **IPC Mechanisms Used**
- **POSIX Shared Memory** (`shm_open` / `mmap`) — Used for the Transaction Pool and Blockchain Ledger.
- **POSIX Semaphores** (`sem_init`) — Ensures mutual exclusion on shared structures.
- **Named Pipe / FIFO** (`mkfifo`) — Handles block delivery from Miner threads to the Validator.
- **System V Message Queue** (`msgget` / `msgsnd` / `msgrcv`) — Facilitates Validator → Statistics communication.
- **Signals** — `SIGINT` for graceful shutdown; `SIGUSR1` to trigger statistics output; `SIGTERM` to terminate child processes.

⚖️ **Dynamic Validator Scaling**  
The Controller thread continuously monitors the transaction pool occupancy and scales Validator processes dynamically to handle the load:

| Pool Occupancy | Active Validators |
| -------------- | ----------------- |
| **< 40%** | 1 |
| **≥ 60%** | 2 |
| **≥ 80%** | 3 |


🔐 **Proof-of-Work & Aging Mechanism**  
- **PoW:** SHA-256 (via OpenSSL) is used to hash each block. The miner increments a nonce until the hash begins with a number of leading zeros proportional to the maximum transaction reward in the block. The Validator independently recalculates and verifies this hash.
- **Anti-Starvation (Aging):** Each time the Validator touches the transaction pool, it increments each pending transaction's `age` counter. Every 50 increments, a transaction's reward increases by 1 — ensuring low-reward transactions are eventually prioritised.

---

## 🚀 Building & Running

⚠️ **IMPORTANT ENVIRONMENT WARNING (WSL USERS)**  
This project heavily relies on Linux POSIX Named Pipes (`mkfifo`). If you are running this project via **Windows Subsystem for Linux (WSL)**, you **MUST** clone and run the project inside the native Linux filesystem (e.g., `~/deichain-blockchain-os`). *Running the project on a mounted Windows drive (like `/mnt/c/Users/...`) will result in an `Operation not supported` error when the system attempts to create the pipe.* 

**Prerequisites**  
- Native Linux Environment (tested on Ubuntu 22.04 / Debian Bookworm)
- GCC with C11 support
- OpenSSL development headers

```bash
# Debian/Ubuntu dependencies installation
sudo apt install build-essential libssl-dev
```

**Build Commands**
```bash
make          # Standard build
make debug    # Build with -DDEBUG for verbose output
make clean    # Remove binaries and object files
```

**Execution Steps**
1. **Start the controller** (*reads config.cfg and launches all processes*):
```bash
./controller
```
2. **Launch transaction generators** (*in separate terminals*):
```bash
# Usage: ./txgen <reward: 1-3> <interval_ms: 200-3000>
./txgen 2 500    # Reward=2, one transaction every 500ms
./txgen 3 200    # Reward=3, one transaction every 200ms
```
3. Interact with the system:
```bash
# Trigger statistics output at any time:
kill -SIGUSR1 <controller_pid>

# Graceful shutdown:
kill -SIGINT <controller_pid>  # Or press Ctrl+C in the controller terminal
```
---

⚙️ **Configuration** (`config.cfg`)
The simulation can be tweaked by modifying the `config.cfg` file before starting the controller.
```Plaintext
5        # NUM_MINERS               — Number of miner threads
50       # TX_POOL_SIZE             — Maximum transactions in the pool
10       # TRANSACTIONS_PER_BLOCK   — Transactions grouped per block
50000    # BLOCKCHAIN_BLOCKS        — Maximum blocks in the ledger
```

---

📈 **Statistics Reported**
When triggered via `SIGUSR1` or upon shutdown, the Statistics process reports:
- Valid and invalid blocks submitted by each miner.
- Total credits earned per miner (sum of transaction rewards).
- Average transaction processing time (creation → ledger insertion).
- Total blocks validated (valid + invalid).
- Total blocks currently in the blockchain.
- Pending transactions at shutdown.

---

📁 **Project Structure**
```Plaintext
deichain-blockchain-os/
├── src/
│   ├── controller.c     # System init, process management, pool monitoring
│   ├── miner.c          # Thread pool, transaction selection, PoW, pipe write
│   ├── validator.c      # Block validation, ledger update, aging mechanism
│   ├── statistics.c     # Metrics collection and reporting
│   ├── txgen.c          # Transaction generator (user process)
│   ├── shared.h         # Shared data structures (Config, Block, Transaction, …)
│   ├── shared.c         # Hash calculation (SHA-256), global pointers
│   ├── logging.h        # Logging interface
│   └── logging.c        # Thread-safe logger (mutex-protected, file + stdout)
├── config.cfg           # Runtime configuration
├── Makefile
└── README.md
```

---

🎓 **Key Concepts Demonstrated**  
- **Multi-process architecture** using `fork` and `execl`.
- **POSIX thread pools** with `pthread_create` and `pthread_join`.
- **Race condition prevention** using `sem_wait` and `sem_post` across shared memory segments.
- **Signal handling** (`sigaction`) for clean teardown and on-demand stats.
- **Blockchain data integrity**: Chaining via `prev_hash`, PoW re-verification, and duplicate transaction detection.

---

👨‍💻 **Authors**  
**Miguel Cunha**  
**Miguel Fernandes**  
*University of Coimbra — DEI Operating Systems, 2024/2025*