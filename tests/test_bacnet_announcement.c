/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <stdio.h>
#include "bacnet_announcement.h"

static int result;
static unsigned sends;
static int send(void *context)
{
    assert(context == &sends);
    sends++;
    return result;
}

int main(void)
{
    bacnet_announcement_t state;
    bacnet_announcement_init(&state);
    result = 31;
    for (unsigned i = 0; i < 100; ++i) {
        bacnet_announcement_tick(&state, i * 1000U, false, send, &sends);
    }
    assert(sends == 0 && state.ready_episodes == 0);
    bacnet_announcement_tick(&state, 100000, true, send, &sends);
    assert(sends == 1 && !state.pending && state.transport_acceptances == 1);
    assert(state.first_attempt_ms == 100000 && state.last_result == 31);
    bacnet_announcement_tick(&state, 1000000, true, send, &sends);
    assert(sends == 1);

    bacnet_announcement_tick(&state, 1000010, false, send, &sends);
    result = -1;
    for (unsigned i = 0; i < 10000; ++i) {
        bacnet_announcement_tick(&state, 1000020 + i, true, send, &sends);
    }
    assert(sends == 6 && state.episode_attempts == 5 && state.exhausted);
    assert(state.failures == 5 && state.ready_episodes == 2);
    assert(state.last_attempt_ms == 1004020 && !state.pending);
    result = 31;
    bacnet_announcement_tick(&state, 2000000, true, send, &sends);
    assert(sends == 6); /* No endless retry after exhaustion. */

    bacnet_announcement_tick(&state, 2000010, false, send, &sends);
    result = 0;
    bacnet_announcement_tick(&state, 2000020, true, send, &sends);
    bacnet_announcement_tick(&state, 2001019, true, send, &sends);
    assert(sends == 7 && state.pending);
    result = 31;
    bacnet_announcement_tick(&state, 2001020, true, send, &sends);
    assert(sends == 8 && !state.pending && !state.exhausted);
    assert(state.transport_acceptances == 2 && state.ready_episodes == 3);
    assert(state.last_accepted_ms == 2001020);
    bacnet_announcement_tick(&state, 3000000, true, send, &sends);
    assert(sends == 8);
    puts("PASS startup gating, send-result diagnostics, bounded retries, link return, no duplicate success");

    bacnet_restart_clock_t clock = {0};
    assert(bacnet_restart_clock_select(&clock, 9000, false, true) == BACNET_RESTART_CLOCK_WAIT);
    assert(!clock.wait_started && !clock.selected);
    assert(bacnet_restart_clock_select(&clock, 10000, true, false) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 14999, true, false) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 15000, true, false) == BACNET_RESTART_CLOCK_FALLBACK);
    assert(clock.selected && !clock.from_valid_clock && clock.selected_ms == 15000);
    assert(bacnet_restart_clock_select(&clock, 16000, true, true) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 17000, false, true) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 18000, true, true) == BACNET_RESTART_CLOCK_WAIT);
    assert(!clock.from_valid_clock);
    clock = (bacnet_restart_clock_t){0};
    assert(bacnet_restart_clock_select(&clock, 0, true, false) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 4999, true, true) == BACNET_RESTART_CLOCK_VALID);
    assert(clock.from_valid_clock && clock.selected_ms == 4999);
    assert(bacnet_restart_clock_select(&clock, 10000, true, false) == BACNET_RESTART_CLOCK_WAIT);
    clock = (bacnet_restart_clock_t){0};
    assert(bacnet_restart_clock_select(&clock, 0, true, false) == BACNET_RESTART_CLOCK_WAIT);
    assert(bacnet_restart_clock_select(&clock, 6000, false, false) == BACNET_RESTART_CLOCK_WAIT);
    assert(!clock.selected);
    assert(bacnet_restart_clock_select(&clock, 7000, true, true) == BACNET_RESTART_CLOCK_VALID);
    puts("PASS bounded clock wait, valid-clock preference, fallback, frozen selection, late sync and link return");
    return 0;
}
