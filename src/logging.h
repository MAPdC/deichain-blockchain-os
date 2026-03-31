/*
Miguel Fernandes | 2023232584
Miguel Cunha | 2021215610
*/

#ifndef LOGGING_H
#define LOGGING_H

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdarg.h>

extern pthread_mutex_t log_mutex;

void init_logger();
void log_message(const char* process_name, const char* message, ...);
void close_logger();

#endif
