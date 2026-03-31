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

#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <stdarg.h>

#define LOG_FILE "DEIChain_log.log"

extern pthread_mutex_t log_mutex;

void init_logger(void);
void log_message(const char* process_name, const char* message, ...);
void close_logger(void);
FILE *log_get_file(void);

#endif
