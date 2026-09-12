/* SPDX-License-Identifier: 0BSD */
#pragma once
#include "FreeRTOS.h"
typedef struct test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
