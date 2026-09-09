/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "firmware.h"

enum {
    FW_INSTANCE_LEGACY = 0,
    FW_INSTANCE_AUTO_LOCKED = 1,
    FW_INSTANCE_MANUAL = 2,
    FW_INSTANCE_AUTO_NEW = 3,
    FW_INSTANCE_AUTO_CHECK_SAVED = 4,
};

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t size;
    uint32_t device_instance;
    uint16_t bacnet_port;
    uint16_t vendor_id;
    uint8_t input_invert_mask;
    bool dhcp_enabled;
    bool restore_relay_state;
    /* Uses the former reserved byte; schema-1 layout/CRC remain compatible. */
    uint8_t device_instance_mode;
    uint32_t database_revision;
    char hostname[FW_HOSTNAME_LEN];
    char device_name[FW_NAME_LEN];
    char vendor_name[FW_VENDOR_NAME_LEN];
    char location[FW_LOCATION_LEN];
    char ip_address[FW_IPV4_TEXT_LEN];
    char netmask[FW_IPV4_TEXT_LEN];
    char gateway[FW_IPV4_TEXT_LEN];
    char dns_server[FW_IPV4_TEXT_LEN];
    char input_names[FW_DI_COUNT][FW_NAME_LEN];
    char relay_names[FW_RELAY_COUNT][FW_NAME_LEN];
    uint32_t crc32;
} firmware_config_t;

void config_model_defaults(firmware_config_t *config);
bool config_model_validate(const firmware_config_t *config, char *reason, size_t reason_size);
uint32_t config_model_crc32(const firmware_config_t *config);
void config_model_finalize(firmware_config_t *config);
bool config_model_is_valid_blob(const firmware_config_t *config);
bool config_model_instance_auto(const firmware_config_t *config);
bool config_model_instance_pending(const firmware_config_t *config);
bool config_model_migrate_instance_mode(firmware_config_t *config);
bool config_model_assign_instance(firmware_config_t *config,
    uint32_t expected_revision, uint32_t instance);
bool config_model_instance_options(firmware_config_t *config,
    bool has_automatic, bool automatic, bool has_instance, uint32_t instance,
    bool reselect);
