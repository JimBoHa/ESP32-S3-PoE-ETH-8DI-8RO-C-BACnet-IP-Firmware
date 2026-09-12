/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "bacnet/bacdef.h"

/* Called only by the BACnet task, under its object mutex. */
typedef struct {
    uint32_t confirmed_timeouts;
    uint32_t refresh_requests;
    uint32_t pending_objects;
    uint32_t capacity_errors;
} bacnet_cov_recovery_stats_t;

void bacnet_cov_recovery_init(void);
void bacnet_cov_recovery_timer_milliseconds(uint32_t elapsed_ms);
bool bacnet_cov_recovery_pending(BACNET_OBJECT_TYPE type, uint32_t instance);
void bacnet_cov_recovery_clear(BACNET_OBJECT_TYPE type, uint32_t instance);
void bacnet_cov_recovery_stats(bacnet_cov_recovery_stats_t *stats);
