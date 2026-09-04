/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void bip_esp32_configure(uint32_t ip_network_order, uint32_t netmask_network_order,
    uint32_t gateway_network_order, uint16_t port);
bool bip_esp32_last_receive_was_broadcast(void);
