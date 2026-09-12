/* SPDX-License-Identifier: 0BSD */
#pragma once
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    (void)(tag); \
    esp_err_t board_test_error = (expression); \
    if (board_test_error != ESP_OK) return board_test_error; \
} while (0)
