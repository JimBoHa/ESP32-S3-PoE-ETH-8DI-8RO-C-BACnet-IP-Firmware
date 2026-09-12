/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BACNET_ANNOUNCEMENT_ATTEMPTS_MAX 5U
#define BACNET_ANNOUNCEMENT_RETRY_MS 1000U
#define BACNET_RESTART_CLOCK_WAIT_MS 5000U

typedef enum {
    BACNET_RESTART_CLOCK_WAIT,
    BACNET_RESTART_CLOCK_VALID,
    BACNET_RESTART_CLOCK_FALLBACK,
} bacnet_restart_clock_choice_t;

typedef struct {
    bool wait_started;
    bool selected;
    bool from_valid_clock;
    uint64_t wait_started_ms;
    uint64_t selected_ms;
} bacnet_restart_clock_t;

/* Pure, nonblocking, once-per-boot selection. Late synchronization or link
   return cannot choose a different timestamp after the first selection. */
bacnet_restart_clock_choice_t bacnet_restart_clock_select(
    bacnet_restart_clock_t *state, uint64_t now_ms, bool ready, bool valid_clock);

typedef struct {
    bool ready;
    bool pending;
    bool exhausted;
    unsigned episode_attempts;
    uint32_t ready_episodes;
    uint32_t attempts;
    uint32_t transport_acceptances;
    uint32_t failures;
    int last_result;
    uint64_t first_attempt_ms;
    uint64_t last_attempt_ms;
    uint64_t last_accepted_ms;
    uint64_t next_attempt_ms;
} bacnet_announcement_t;

/* One bounded I-Am campaign per completed-startup/network-ready episode.
   A positive result means local transport acceptance, never a remote ACK.
   Link return may announce identity again; this model never creates a restart. */
void bacnet_announcement_init(bacnet_announcement_t *state);
void bacnet_announcement_tick(bacnet_announcement_t *state, uint64_t now_ms,
    bool ready, int (*send)(void *context), void *context);
