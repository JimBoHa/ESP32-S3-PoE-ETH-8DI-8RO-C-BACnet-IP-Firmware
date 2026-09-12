/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

#define CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES 256U

/* Separate from firmware_config_t so older images retain the original identity
   blob unchanged. Payload is canonical BACnet recipient-list encoding, validated
   by the caller. NOT_FOUND differs from a successfully loaded empty list.
   Getter changes encoded only on success and sets *length=0 on failure. */
esp_err_t config_store_restart_recipients_get(
    uint8_t *encoded, size_t capacity, size_t *length);
/* A successful identical write does not touch flash. No runtime recipient list
   is published here; the caller must publish its candidate only after ESP_OK. */
esp_err_t config_store_restart_recipients_set(const uint8_t *encoded, size_t length);
