/* SPDX-License-Identifier: 0BSD */
#include "bacnet_instance.h"

#include <string.h>

static bool in_range(uint32_t instance)
{
    return instance >= BACNET_INSTANCE_FIRST && instance <= BACNET_INSTANCE_LAST;
}

static uint32_t next_random(bacnet_instance_t *self)
{
    uint32_t value = self->random;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    self->random = value;
    return value;
}

static void mark_occupied(bacnet_instance_t *self, uint32_t instance)
{
    if (in_range(instance)) {
        unsigned index = instance - BACNET_INSTANCE_FIRST;
        self->occupied[index / 8U] |= (uint8_t)(1U << (index % 8U));
    }
}

static bool occupied(const bacnet_instance_t *self, uint32_t instance)
{
    if (!in_range(instance)) {
        return true;
    }
    unsigned index = instance - BACNET_INSTANCE_FIRST;
    return (self->occupied[index / 8U] & (1U << (index % 8U))) != 0;
}

static void discover(bacnet_instance_t *self, uint64_t now_ms)
{
    memset(self->occupied, 0, sizeof(self->occupied));
    self->state = INSTANCE_DISCOVERING;
    /* Three Who-Is rounds, with jitter to spread simultaneous power-up. */
    self->next_query_ms = now_ms + next_random(self) % 501U;
    self->deadline_ms = self->next_query_ms + 3000U;
}

void bacnet_instance_start(bacnet_instance_t *self, bool automatic,
    uint32_t preferred, uint32_t seed, uint64_t address_key, uint64_t now_ms)
{
    memset(self, 0, sizeof(*self));
    self->automatic = automatic;
    self->preferred = preferred;
    self->candidate = preferred;
    self->address_key = address_key;
    self->random = seed ^ (uint32_t)(address_key >> 16) ^ (uint32_t)address_key;
    if (!self->random) {
        self->random = 0x9e3779b9U;
    }
    if (automatic) {
        discover(self, now_ms);
    } else {
        self->state = INSTANCE_READY;
        self->next_query_ms = now_ms;
    }
}

void bacnet_instance_observe(bacnet_instance_t *self, uint32_t instance,
    uint64_t address_key, uint64_t now_ms)
{
    if (address_key == self->address_key) {
        return;
    }
    mark_occupied(self, instance);
    if (instance != self->candidate ||
        (self->state != INSTANCE_CLAIMING && self->state != INSTANCE_READY)) {
        return;
    }
    self->conflict = true;
    self->conflicts++;
    if (!self->automatic) {
        return; /* Locked automatic and manual assignments stay fixed. */
    }
    uint32_t duplicate = self->candidate;
    self->preferred = BACNET_INSTANCE_NONE;
    discover(self, now_ms);
    mark_occupied(self, duplicate);
}

unsigned bacnet_instance_tick(bacnet_instance_t *self, uint64_t now_ms)
{
    if (self->state == INSTANCE_RANGE_FULL && now_ms >= self->deadline_ms) {
        discover(self, now_ms);
    }
    if (self->state == INSTANCE_DISCOVERING && now_ms >= self->deadline_ms) {
        uint32_t candidate = self->preferred;
        if (occupied(self, candidate)) {
            unsigned start = next_random(self) % BACNET_INSTANCE_COUNT;
            candidate = BACNET_INSTANCE_NONE;
            for (unsigned i = 0; i < BACNET_INSTANCE_COUNT; i++) {
                uint32_t next = BACNET_INSTANCE_FIRST + (start + i) % BACNET_INSTANCE_COUNT;
                if (!occupied(self, next)) {
                    candidate = next;
                    break;
                }
            }
        }
        if (candidate == BACNET_INSTANCE_NONE) {
            self->state = INSTANCE_RANGE_FULL;
            self->deadline_ms = now_ms + 30000U;
            return 0;
        }
        self->candidate = candidate;
        self->conflict = false;
        self->state = INSTANCE_CLAIMING;
        self->deadline_ms = now_ms + 3000U;
        self->next_query_ms = now_ms + 1000U;
        return INSTANCE_ANNOUNCE | INSTANCE_QUERY;
    }
    if (self->state == INSTANCE_CLAIMING && now_ms >= self->deadline_ms) {
        self->state = INSTANCE_SAVING;
        return INSTANCE_SAVE;
    }
    if ((self->state == INSTANCE_DISCOVERING || self->state == INSTANCE_CLAIMING ||
         self->state == INSTANCE_READY) && now_ms >= self->next_query_ms) {
        self->next_query_ms = now_ms + (self->state == INSTANCE_READY ? 60000U : 1000U);
        return INSTANCE_QUERY | (self->state == INSTANCE_CLAIMING ? INSTANCE_ANNOUNCE : 0U);
    }
    return 0;
}

void bacnet_instance_saved(bacnet_instance_t *self, bool success, uint64_t now_ms)
{
    if (self->state != INSTANCE_SAVING) {
        return;
    }
    self->state = success ? INSTANCE_READY : INSTANCE_SAVE_FAILED;
    if (success) {
        self->automatic = false; /* Assignment locks immediately, before use. */
        self->preferred = self->candidate;
        self->conflict = false;
        self->next_query_ms = now_ms + 500U;
    }
}

const char *bacnet_instance_state_name(const bacnet_instance_t *self)
{
    switch (self->state) {
        case INSTANCE_DISCOVERING: return "discovering";
        case INSTANCE_CLAIMING: return "checking-candidate";
        case INSTANCE_SAVING: return "saving";
        case INSTANCE_READY:
            return self->conflict ? "locked-conflict" : "locked";
        case INSTANCE_RANGE_FULL: return "range-full";
        case INSTANCE_SAVE_FAILED: return "save-failed";
        case INSTANCE_CONFIG_CHANGED: return "reboot-required";
    }
    return "unknown";
}
