/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"

esp_err_t bacnet_app_start(const firmware_config_t *config);
bool bacnet_app_running(void);
uint32_t bacnet_app_packet_count(void);

typedef enum {
    BACNET_RELAY_COMMAND_OFF = 0,
    BACNET_RELAY_COMMAND_ON,
    BACNET_RELAY_COMMAND_RELINQUISH,
} bacnet_relay_command_t;

typedef struct {
    bool active;
    unsigned active_priority;
} bacnet_relay_status_t;

esp_err_t bacnet_app_relay_command(unsigned index, bacnet_relay_command_t command,
    unsigned priority, bacnet_relay_status_t *status);
bool bacnet_app_relay_priorities(unsigned priorities[FW_RELAY_COUNT]);
