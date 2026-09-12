/* SPDX-License-Identifier: 0BSD */
#include "clock_model.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    clock_model_t m = {0};
    int64_t now = -1, boot = -1;
    assert(!clock_model_sample(&m, 0, &now, &boot));
    assert(now == -1 && boot == -1);
    assert(!clock_model_time_valid(CLOCK_MIN_UNIX_SECONDS - 1, 0));
    assert(!clock_model_time_valid(CLOCK_MAX_UNIX_SECONDS, 0));
    assert(!clock_model_time_valid(INT64_MAX, INT64_MAX));
    assert(!clock_model_time_valid(1700000000, -1));
    assert(!clock_model_time_valid(1700000000, 1000000));
    assert(clock_model_time_valid(CLOCK_MIN_UNIX_SECONDS, 0));
    assert(clock_model_time_valid(CLOCK_MAX_UNIX_SECONDS - 1, 999999));
    assert(clock_model_accept(&m, 1700000000, 123456, 5000000, true));
    assert(m.sync_count == 1 && m.network_sync && m.valid);
    assert(clock_model_sample(&m, 6000000, &now, &boot));
    assert(now == INT64_C(1700000001123456));
    assert(boot == INT64_C(1699999995123456));
    int64_t frozen_boot = boot;
    assert(clock_model_accept(&m, 1700000020, 123456, 10000000, true));
    assert(clock_model_sample(&m, 10000000, &now, &boot));
    assert(boot == INT64_C(1700000010123456) && frozen_boot != boot);
    /* A caller freezes once; later backward/forward corrections are ordinary
       clock syncs, not invented restart notifications. */
    assert(clock_model_accept(&m, 1700000019, 0, 11000000, true));
    assert(clock_model_sample(&m, 12000000, &now, &boot));
    assert(now == INT64_C(1700000020000000));
    assert(!clock_model_sample(&m, 10999999, &now, &boot));
    assert(!clock_model_sample(&m, INT64_MAX, &now, &boot));
    clock_model_t saved = m;
    assert(!clock_model_accept(&m, INT64_MAX, 0, 1, true));
    assert(!clock_model_accept(&m, 1700000019, 0, -1, true));
    assert(!clock_model_accept(&m, CLOCK_MIN_UNIX_SECONDS, 0, 1, true));
    assert(m.anchor_unix_us == saved.anchor_unix_us && m.sync_count == saved.sync_count);
    assert(m.rejected_syncs == saved.rejected_syncs + 3);
    now = INT64_C(1700000000000000);
    assert(clock_model_holdover_valid(now, now));
    assert(clock_model_holdover_valid(now + CLOCK_HOLDOVER_MAX_US, now));
    assert(!clock_model_holdover_valid(now + CLOCK_HOLDOVER_MAX_US + 1, now));
    assert(!clock_model_holdover_valid(now - 1, now));
    assert(!clock_model_holdover_valid(INT64_MAX, now));
    memset(&m, 0, sizeof(m));
    assert(clock_model_accept(&m, 1700000000, 0, 100000, false));
    assert(m.valid && !m.network_sync && m.sync_count == 0);
    assert(clock_model_accept(&m, 1700000001, 0, 1100000, true));
    assert(m.network_sync && m.sync_count == 1);
    assert(clock_model_retained_checksum(now) == clock_model_retained_checksum(now));
    assert(clock_model_retained_checksum(now) != clock_model_retained_checksum(now + 1));
    puts("PASS clock model: range/microseconds, monotonic anchor, boot derivation, corrections, rejection, holdover age/CRC");
    return 0;
}
