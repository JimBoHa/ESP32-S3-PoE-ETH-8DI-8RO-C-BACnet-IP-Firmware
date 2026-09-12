/* SPDX-License-Identifier: 0BSD */
#include "bacnet_announcement.h"

#include <string.h>

bacnet_restart_clock_choice_t bacnet_restart_clock_select(
    bacnet_restart_clock_t *state, uint64_t now_ms, bool ready, bool valid_clock)
{
    if (!state || !ready || state->selected) {
        return BACNET_RESTART_CLOCK_WAIT;
    }
    if (!state->wait_started) {
        state->wait_started = true;
        state->wait_started_ms = now_ms;
    }
    if (!valid_clock && now_ms - state->wait_started_ms < BACNET_RESTART_CLOCK_WAIT_MS) {
        return BACNET_RESTART_CLOCK_WAIT;
    }
    state->selected = true;
    state->from_valid_clock = valid_clock;
    state->selected_ms = now_ms;
    return valid_clock ? BACNET_RESTART_CLOCK_VALID : BACNET_RESTART_CLOCK_FALLBACK;
}

void bacnet_announcement_init(bacnet_announcement_t *state)
{
    if (state) {
        memset(state, 0, sizeof(*state));
    }
}

void bacnet_announcement_tick(bacnet_announcement_t *state, uint64_t now_ms,
    bool ready, int (*send)(void *context), void *context)
{
    if (!state) {
        return;
    }
    if (!ready) {
        state->ready = false;
        state->pending = false;
        return;
    }
    if (!state->ready) {
        state->ready = true;
        state->pending = true;
        state->exhausted = false;
        state->episode_attempts = 0;
        state->ready_episodes++;
        state->next_attempt_ms = now_ms;
    }
    if (!state->pending || now_ms < state->next_attempt_ms) {
        return;
    }
    if (!state->attempts) {
        state->first_attempt_ms = now_ms;
    }
    state->attempts++;
    state->episode_attempts++;
    state->last_attempt_ms = now_ms;
    state->last_result = send ? send(context) : -1;
    if (state->last_result > 0) {
        state->transport_acceptances++;
        state->last_accepted_ms = now_ms;
        state->pending = false;
    } else {
        state->failures++;
        if (state->episode_attempts >= BACNET_ANNOUNCEMENT_ATTEMPTS_MAX) {
            state->pending = false;
            state->exhausted = true;
        } else {
            state->next_attempt_ms = now_ms + BACNET_ANNOUNCEMENT_RETRY_MS;
        }
    }
}
