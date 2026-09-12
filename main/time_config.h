/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define TIME_CONFIG_NTP_SERVER_SIZE 254U
#define TIME_CONFIG_TIMEZONE_SIZE 96U
#define TIME_CONFIG_DEFAULT_NTP_SERVER "pool.ntp.org"
#define TIME_CONFIG_DEFAULT_TIMEZONE "UTC0"

typedef struct {
    char ntp_server[TIME_CONFIG_NTP_SERVER_SIZE];
    char timezone[TIME_CONFIG_TIMEZONE_SIZE];
} time_config_t;

/* Separate versioned NVS value: never changes firmware_config_t or its CRC. */
esp_err_t time_config_init(void);
void time_config_get(time_config_t *config);
/* Commits before publishing; identical saved values do not wear flash. */
esp_err_t time_config_update(const time_config_t *config);
bool time_config_validate(const time_config_t *config, char *reason, size_t reason_size);
