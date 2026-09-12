/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    bool valid;
    int64_t utc_unix_us;
    int64_t boot_utc_unix_us;
    struct tm local_time;
    struct tm boot_local_time;
    /* BACnet convention: minutes added to LOCAL STANDARD time to obtain UTC.
       DST is reported separately; e.g. Pacific is +480 even in summer. */
    int16_t utc_offset_minutes;
    bool daylight_saving;
    const char *source; /* static literal: ntp, retained-ntp, unsynchronized */
} clock_service_snapshot_t;

typedef struct {
    bool valid;
    bool synchronized; /* an accepted NTP response this boot, not just holdover */
    bool sntp_running;
    const char *source;
    uint32_t sync_count;
    uint32_t rejected_syncs;
    uint32_t start_failures;
    uint32_t config_generation;
    int64_t last_sync_unix_us;
    uint64_t last_sync_uptime_ms;
    uint64_t sync_age_seconds;
    char utc_time[40];
    char local_time[40];
} clock_service_status_t;

/* Initialize after esp_netif/event loop/Ethernet and time_config_init.
   Never waits for DNS/NTP and never blocks BACnet or the relay task. */
esp_err_t clock_service_init(void);
/* Wake its sole SNTP/config owner after persisted configuration changes. */
void clock_service_config_changed(void);
/* Lock-bounded RAM-only snapshots; no DNS, socket, I2C or flash operations.
   The caller freezes its chosen boot timestamp; later clock corrections do
   not constitute a new restart notification. */
bool clock_service_snapshot_get(clock_service_snapshot_t *snapshot);
bool clock_service_status_get(clock_service_status_t *status);
