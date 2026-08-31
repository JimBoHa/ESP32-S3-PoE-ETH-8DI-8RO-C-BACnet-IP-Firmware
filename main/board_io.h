/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"

esp_err_t board_io_init(const firmware_config_t *config);
bool board_io_input_get(unsigned index);
uint8_t board_io_inputs_mask(void);

esp_err_t board_io_relay_set(unsigned index, bool active);
bool board_io_relay_get(unsigned index);
uint8_t board_io_relays_mask(void);
uint8_t board_io_relay_commands_mask(void);

bool board_io_relay_controller_healthy(void);
bool board_io_rtc_present(void);
