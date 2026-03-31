/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#include "logging.h"
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_FILE "DEIChain_log.txt"

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
FILE* log_file = NULL;

void init_logger() {
    pthread_mutex_lock(&log_mutex);
    log_file = fopen(LOG_FILE, "a");
    if (log_file == NULL) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_message(const char* process_name, const char* format, ...) {
    time_t now;
    time(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    
    pthread_mutex_lock(&log_mutex);
    
    printf("[%s][%s][PID:%d] %s\n", time_str, process_name, getpid(), message);
    
    if (log_file) {
        fprintf(log_file, "[%s][%s][PID:%d] %s\n", time_str, process_name, getpid(), message);
        fflush(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

void close_logger() {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}
