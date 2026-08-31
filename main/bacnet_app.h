/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"

esp_err_t bacnet_app_start(const firmware_config_t *config);
bool bacnet_app_running(void);
uint32_t bacnet_app_packet_count(void);
