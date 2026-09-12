/* SPDX-License-Identifier: 0BSD */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CLOCK_MIN_UNIX_SECONDS INT64_C(1577836800) /* 2020-01-01 */
#define CLOCK_MAX_UNIX_SECONDS INT64_C(4102444800) /* 2100-01-01, exclusive */
#define CLOCK_HOLDOVER_MAX_US (INT64_C(7) * 86400 * 1000000)

typedef struct {
    bool valid;
    bool network_sync;
    int64_t anchor_unix_us;
    int64_t anchor_uptime_us;
    int64_t last_sync_unix_us;
    int64_t last_sync_uptime_us;
    uint32_t sync_count;
    uint32_t rejected_syncs;
} clock_model_t;

bool clock_model_time_valid(int64_t seconds, int64_t microseconds);
bool clock_model_accept(clock_model_t *model, int64_t seconds,
    int64_t microseconds, int64_t uptime_us, bool network_sync);
bool clock_model_sample(const clock_model_t *model, int64_t uptime_us,
    int64_t *utc_us, int64_t *boot_utc_us);
bool clock_model_holdover_valid(int64_t now_us, int64_t last_sync_us);
uint32_t clock_model_retained_checksum(int64_t last_sync_us);
