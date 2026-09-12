/* SPDX-License-Identifier: 0BSD */
#pragma once
#include <stdbool.h>
#include <sys/time.h>
#include "esp_err.h"
typedef struct {
    bool smooth_sync, server_from_dhcp, wait_for_sync, start;
    void (*sync_cb)(struct timeval *);
    size_t num_of_servers;
    const char *servers[1];
} esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) { .smooth_sync = false, \
    .server_from_dhcp = false, .wait_for_sync = true, .start = true, \
    .sync_cb = NULL, .num_of_servers = 1, .servers = {server} }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config);
void esp_netif_sntp_deinit(void);
esp_err_t esp_netif_sntp_start(void);
