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

#include "logging.h"
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE* log_file = NULL;

void init_logger(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file == NULL) {
        log_file = fopen(LOG_FILE, "a");
        if (log_file == NULL) {
            perror("Failed to open log file");
        }
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_message(const char* process_name, const char* format, ...) {
    time_t now = time(NULL);
    char time_str[10];
    strftime(time_str, sizeof(time_str), "%H:%M:%S", localtime(&now));
    
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    pthread_mutex_lock(&log_mutex);
    
    printf("%s %s: %s\n", time_str, process_name, message);
    
    if (log_file) {
        fprintf(log_file, "%s %s: %s\n", time_str, process_name, message);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void close_logger(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

FILE *log_get_file(void) {
    return log_file;
}