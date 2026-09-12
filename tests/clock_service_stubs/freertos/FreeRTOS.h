/* SPDX-License-Identifier: 0BSD */
#pragma once
#include <stdint.h>
typedef int BaseType_t;
typedef unsigned TickType_t;
typedef struct { unsigned entered; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED {0}
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMAX_DELAY ((TickType_t)-1)
void clock_test_enter_critical(portMUX_TYPE *lock);
void clock_test_exit_critical(portMUX_TYPE *lock);
#define portENTER_CRITICAL(lock) clock_test_enter_critical(lock)
#define portEXIT_CRITICAL(lock) clock_test_exit_critical(lock)
