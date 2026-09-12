/* SPDX-License-Identifier: 0BSD */
#pragma once
typedef enum { ESP_RST_POWERON, ESP_RST_SW, ESP_RST_DEEPSLEEP,
    ESP_RST_BROWNOUT, ESP_RST_PANIC, ESP_RST_WDT } esp_reset_reason_t;
esp_reset_reason_t esp_reset_reason(void);
