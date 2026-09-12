/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"

typedef struct {
    uint8_t desired_mask;
    uint8_t applied_mask;
    uint8_t output_register;
    uint8_t configuration_register;
    bool registers_valid;
    bool healthy;
    uint32_t i2c_errors;
    uint32_t verification_failures;
    uint32_t configuration_recoveries;
    uint32_t mutex_timeouts;
    esp_err_t last_error;
    uint64_t last_verified_ms;
} board_io_relay_diagnostics_t;

esp_err_t board_io_init(const firmware_config_t *config);
bool board_io_input_get(unsigned index);
uint8_t board_io_inputs_mask(void);

esp_err_t board_io_relay_set(unsigned index, bool active);
bool board_io_relay_get(unsigned index);
uint8_t board_io_relays_mask(void);
uint8_t board_io_relay_commands_mask(void);
/* Applied/register values describe the expander, never relay contact feedback.
   On failure applied_mask remains the last verified value, with healthy false. */
bool board_io_relay_diagnostics_get(board_io_relay_diagnostics_t *diagnostics);

bool board_io_relay_controller_healthy(void);
bool board_io_rtc_present(void);
