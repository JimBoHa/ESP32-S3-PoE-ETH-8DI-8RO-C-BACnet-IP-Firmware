/* SPDX-License-Identifier: 0BSD */
#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
    unsigned stack_depth, void *context, BaseType_t priority, TaskHandle_t *handle);
void vTaskDelay(TickType_t ticks);
