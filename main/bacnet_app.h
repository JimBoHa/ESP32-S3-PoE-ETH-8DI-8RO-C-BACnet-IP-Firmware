/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "config_model.h"
#include "bacnet_command_trace.h"
#include "bacnet_cov_recovery.h"
#include "bacnet_restart.h"
#include "bacnet_announcement.h"

typedef struct {
    bool timestamp_frozen;
    bool timestamp_from_valid_clock;
    const char *timestamp_source;
    BACNET_TIMESTAMP timestamp;
    uint64_t wait_started_ms;
    uint64_t selected_ms;
} bacnet_app_time_stats_t;

esp_err_t bacnet_app_start(const firmware_config_t *config);
void bacnet_app_startup_complete(void);
bool bacnet_app_restart_stats_get(bacnet_restart_stats_t *stats);
bool bacnet_app_announcement_stats_get(bacnet_announcement_t *stats);
bool bacnet_app_time_stats_get(bacnet_app_time_stats_t *stats);
bool bacnet_app_running(void);
uint32_t bacnet_app_packet_count(void);
uint32_t bacnet_app_device_instance(void);
const char *bacnet_app_instance_status(void);
uint32_t bacnet_app_instance_conflicts(void);
bool bacnet_app_command_trace_get(bacnet_command_trace_snapshot_t *snapshot);
bool bacnet_app_cov_recovery_get(bacnet_cov_recovery_stats_t *stats);

typedef enum {
    BACNET_RELAY_COMMAND_OFF = 0,
    BACNET_RELAY_COMMAND_ON,
    BACNET_RELAY_COMMAND_RELINQUISH,
} bacnet_relay_command_t;

typedef struct {
    bool active;
    unsigned active_priority;
} bacnet_relay_status_t;

esp_err_t bacnet_app_relay_command(unsigned index, bacnet_relay_command_t command,
    unsigned priority, bacnet_relay_status_t *status);
bool bacnet_app_relay_priorities(unsigned priorities[FW_RELAY_COUNT]);
