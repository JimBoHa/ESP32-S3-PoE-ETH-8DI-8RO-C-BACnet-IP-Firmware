/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BACNET_INSTANCE_FIRST 599000U
#define BACNET_INSTANCE_COUNT 1000U
#define BACNET_INSTANCE_LAST (BACNET_INSTANCE_FIRST + BACNET_INSTANCE_COUNT - 1U)
#define BACNET_INSTANCE_NONE UINT32_MAX

typedef enum {
    INSTANCE_DISCOVERING,
    INSTANCE_CLAIMING,
    INSTANCE_SAVING,
    INSTANCE_READY,
    INSTANCE_RANGE_FULL,
    INSTANCE_SAVE_FAILED,
    INSTANCE_CONFIG_CHANGED,
} bacnet_instance_state_t;

enum {
    INSTANCE_QUERY = 1U,
    INSTANCE_ANNOUNCE = 2U,
    INSTANCE_SAVE = 4U,
};

typedef struct {
    uint8_t occupied[(BACNET_INSTANCE_COUNT + 7U) / 8U];
    bacnet_instance_state_t state;
    bool automatic;
    bool conflict;
    uint32_t candidate;
    uint32_t preferred;
    uint32_t random;
    uint32_t conflicts;
    uint64_t address_key;
    uint64_t deadline_ms;
    uint64_t next_query_ms;
} bacnet_instance_t;

void bacnet_instance_start(bacnet_instance_t *self, bool automatic,
    uint32_t preferred, uint32_t seed, uint64_t address_key, uint64_t now_ms);
void bacnet_instance_observe(bacnet_instance_t *self, uint32_t instance,
    uint64_t address_key, uint64_t now_ms);
unsigned bacnet_instance_tick(bacnet_instance_t *self, uint64_t now_ms);
void bacnet_instance_saved(bacnet_instance_t *self, bool success, uint64_t now_ms);
const char *bacnet_instance_state_name(const bacnet_instance_t *self);
