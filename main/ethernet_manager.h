/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "config_model.h"

esp_err_t ethernet_manager_init(const firmware_config_t *config);
bool ethernet_manager_wait_for_ip(uint32_t timeout_ms);
bool ethernet_manager_link_up(void);
bool ethernet_manager_has_ip(void);
uint32_t ethernet_manager_network_revision(void);
bool ethernet_manager_get_ip_info(esp_netif_ip_info_t *info);
bool ethernet_manager_get_dns_info(esp_netif_dns_info_t *info);
void ethernet_manager_mac_get(uint8_t mac[6]);
