/* SPDX-License-Identifier: 0BSD */
#pragma once
#include <stdint.h>
typedef struct { uint32_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { uint32_t ip; } esp_netif_dns_info_t;
