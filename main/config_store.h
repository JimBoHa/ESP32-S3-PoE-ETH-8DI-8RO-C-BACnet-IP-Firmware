/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"

esp_err_t config_store_init(void);
void config_store_get(firmware_config_t *config);
esp_err_t config_store_update(const firmware_config_t *config);
esp_err_t config_store_assign_instance(uint32_t expected_revision,
    uint32_t instance, firmware_config_t *saved);

esp_err_t config_store_admin_key_get(uint8_t key[FW_AUTH_KEY_BYTES], bool *created);
uint32_t config_store_reboot_count(void);

uint8_t config_store_relay_state_get(void);
esp_err_t config_store_relay_state_set(uint8_t relay_state);
