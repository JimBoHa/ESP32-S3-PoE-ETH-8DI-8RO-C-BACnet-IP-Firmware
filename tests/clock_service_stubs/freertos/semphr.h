/* SPDX-License-Identifier: 0BSD */
#pragma once
#include "FreeRTOS.h"
typedef struct clock_test_mutex *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
