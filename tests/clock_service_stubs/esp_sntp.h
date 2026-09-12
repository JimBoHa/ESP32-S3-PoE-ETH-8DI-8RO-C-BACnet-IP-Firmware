/* SPDX-License-Identifier: 0BSD */
#pragma once
#include "lwip/ip_addr.h"
#define SNTP_SYNC_STATUS_COMPLETED 1
void sntp_set_sync_status(int status);
void esp_sntp_stop(void);
void esp_sntp_setserver(uint8_t index, const ip_addr_t *address);
