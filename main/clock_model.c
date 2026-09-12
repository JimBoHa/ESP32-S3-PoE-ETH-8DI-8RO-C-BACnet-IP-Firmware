/* SPDX-License-Identifier: 0BSD */
#include "clock_model.h"
#include <stddef.h>
#include <limits.h>

bool clock_model_time_valid(int64_t seconds, int64_t microseconds)
{
    return seconds >= CLOCK_MIN_UNIX_SECONDS && seconds < CLOCK_MAX_UNIX_SECONDS &&
        microseconds >= 0 && microseconds < 1000000;
}

bool clock_model_accept(clock_model_t *m, int64_t seconds, int64_t microseconds,
    int64_t uptime_us, bool network_sync)
{
    if (!m) return false;
    if (!clock_model_time_valid(seconds, microseconds) || uptime_us < 0 ||
        uptime_us > seconds * INT64_C(1000000) + microseconds -
            CLOCK_MIN_UNIX_SECONDS * INT64_C(1000000)) {
        m->rejected_syncs++;
        return false;
    }
    m->valid = true;
    m->network_sync = network_sync;
    m->anchor_unix_us = seconds * INT64_C(1000000) + microseconds;
    m->anchor_uptime_us = uptime_us;
    if (network_sync) {
        m->sync_count++;
        m->last_sync_unix_us = m->anchor_unix_us;
        m->last_sync_uptime_us = uptime_us;
    }
    return true;
}

bool clock_model_sample(const clock_model_t *m, int64_t uptime_us,
    int64_t *utc_us, int64_t *boot_utc_us)
{
    if (!m || !m->valid || !utc_us || !boot_utc_us || uptime_us < m->anchor_uptime_us)
        return false;
    int64_t delta = uptime_us - m->anchor_uptime_us;
    if (delta > INT64_MAX - m->anchor_unix_us) return false;
    int64_t now = m->anchor_unix_us + delta;
    if (!clock_model_time_valid(now / 1000000, now % 1000000)) return false;
    *utc_us = now;
    *boot_utc_us = m->anchor_unix_us - m->anchor_uptime_us;
    return true;
}

bool clock_model_holdover_valid(int64_t now_us, int64_t last_sync_us)
{
    return clock_model_time_valid(now_us / 1000000, now_us % 1000000) &&
        clock_model_time_valid(last_sync_us / 1000000, last_sync_us % 1000000) &&
        now_us >= last_sync_us && now_us - last_sync_us <= CLOCK_HOLDOVER_MAX_US;
}

uint32_t clock_model_retained_checksum(int64_t last_sync_us)
{
    /* CRC-32, little-endian timestamp; marker format has its own magic/version. */
    uint64_t value = (uint64_t)last_sync_us;
    uint32_t crc = UINT32_MAX;
    for (unsigned i = 0; i < 8; ++i) {
        crc ^= (uint8_t)(value >> (i * 8));
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
    }
    return ~crc;
}
