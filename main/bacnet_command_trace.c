/* SPDX-License-Identifier: 0BSD */
#include "bacnet_command_trace.h"

#include <string.h>

/* BACnet confirmed-service wire identifiers. Kept independent of the stack
   so the bounded recorder can be tested on the host without an RTOS. */
#define WRITE_PROPERTY 15U
#define WRITE_PROPERTY_MULTIPLE 16U

static bacnet_command_trace_snapshot_t s_trace;
static unsigned s_next;
static bool s_pending;
static bool s_processing_enabled;
static uint8_t s_peer[BACNET_COMMAND_TRACE_PEER_BYTES];
static uint8_t s_invoke_id;
static uint8_t s_service_choice;

void bacnet_command_trace_reset(void)
{
    memset(&s_trace, 0, sizeof(s_trace));
    s_next = 0;
    s_pending = false;
}

void bacnet_command_trace_end(void)
{
    s_pending = false;
}

static void append_record(const uint8_t *apdu, uint16_t length,
    uint64_t uptime_ms, bool outgoing, int send_result)
{
    bacnet_command_trace_record_t *record = &s_trace.records[s_next];
    memset(record, 0, sizeof(*record));
    record->sequence = ++s_trace.total_records;
    record->uptime_ms = uptime_ms;
    record->apdu_length = length;
    record->captured_length = length < BACNET_COMMAND_TRACE_APDU_BYTES ?
        length : BACNET_COMMAND_TRACE_APDU_BYTES;
    record->outgoing = outgoing;
    record->processing_enabled = s_processing_enabled;
    record->service_choice = s_service_choice;
    record->send_result = send_result;
    memcpy(record->peer, s_peer, sizeof(record->peer));
    memcpy(record->apdu, apdu, record->captured_length);
    s_next = (s_next + 1U) % BACNET_COMMAND_TRACE_CAPACITY;
    if (s_trace.retained_records < BACNET_COMMAND_TRACE_CAPACITY) {
        s_trace.retained_records++;
    }
}

void bacnet_command_trace_begin(const uint8_t *peer, size_t peer_length,
    const uint8_t *apdu, uint16_t apdu_length, uint64_t uptime_ms,
    bool processing_enabled)
{
    s_pending = false;
    if (!peer || peer_length != sizeof(s_peer) || !apdu || apdu_length < 4U ||
        (apdu[0] & 0xf0U) != 0x00U) {
        return;
    }
    unsigned service_offset = (apdu[0] & 0x08U) ? 5U : 3U;
    if (apdu_length <= service_offset) {
        return;
    }
    uint8_t service = apdu[service_offset];
    if (service != WRITE_PROPERTY && service != WRITE_PROPERTY_MULTIPLE) {
        return;
    }
    memcpy(s_peer, peer, sizeof(s_peer));
    s_invoke_id = apdu[2];
    s_service_choice = service;
    s_processing_enabled = processing_enabled;
    s_pending = true;
    if (service == WRITE_PROPERTY) {
        s_trace.write_property_requests++;
    } else {
        s_trace.write_property_multiple_requests++;
    }
    append_record(apdu, apdu_length, uptime_ms, false, 0);
}

void bacnet_command_trace_response(const uint8_t *peer, size_t peer_length,
    const uint8_t *apdu, uint16_t apdu_length, uint64_t uptime_ms, int send_result)
{
    if (!s_pending || !s_processing_enabled || !peer ||
        peer_length != sizeof(s_peer) || memcmp(peer, s_peer, sizeof(s_peer)) ||
        !apdu || apdu_length < 3U || apdu[1] != s_invoke_id) {
        return;
    }
    uint8_t type = apdu[0] & 0xf0U;
    if (type == 0x20U || type == 0x50U) {
        if (apdu[2] != s_service_choice) {
            return;
        }
    } else if (type != 0x60U && type != 0x70U) {
        return;
    }
    append_record(apdu, apdu_length, uptime_ms, true, send_result);
}

void bacnet_command_trace_snapshot(bacnet_command_trace_snapshot_t *snapshot)
{
    if (!snapshot) {
        return;
    }
    *snapshot = s_trace;
    unsigned oldest = (s_next + BACNET_COMMAND_TRACE_CAPACITY -
        s_trace.retained_records) % BACNET_COMMAND_TRACE_CAPACITY;
    for (unsigned i = 0; i < s_trace.retained_records; ++i) {
        snapshot->records[i] = s_trace.records[
            (oldest + i) % BACNET_COMMAND_TRACE_CAPACITY];
    }
}
