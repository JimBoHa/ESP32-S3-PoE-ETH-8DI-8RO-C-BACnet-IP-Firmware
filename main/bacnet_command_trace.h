/* SPDX-License-Identifier: 0BSD */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BACNET_COMMAND_TRACE_CAPACITY 8U
#define BACNET_COMMAND_TRACE_APDU_BYTES 96U
#define BACNET_COMMAND_TRACE_PEER_BYTES 6U

typedef struct {
    uint64_t sequence;
    uint64_t uptime_ms;
    uint16_t apdu_length;
    uint16_t captured_length;
    int32_t send_result;
    bool outgoing;
    bool processing_enabled;
    uint8_t service_choice;
    uint8_t peer[BACNET_COMMAND_TRACE_PEER_BYTES];
    uint8_t apdu[BACNET_COMMAND_TRACE_APDU_BYTES];
} bacnet_command_trace_record_t;

typedef struct {
    uint64_t total_records;
    uint64_t write_property_requests;
    uint64_t write_property_multiple_requests;
    unsigned retained_records;
    bacnet_command_trace_record_t records[BACNET_COMMAND_TRACE_CAPACITY];
} bacnet_command_trace_snapshot_t;

/* The caller serializes all access. Begin/end bracket one synchronous NPDU
   dispatch; responses from a later transaction must never inherit its context.
   Peer is the six-byte BACnet/IP address, not an authenticated writer identity. */
void bacnet_command_trace_reset(void);
void bacnet_command_trace_begin(const uint8_t *peer, size_t peer_length,
    const uint8_t *apdu, uint16_t apdu_length, uint64_t uptime_ms,
    bool processing_enabled);
void bacnet_command_trace_end(void);
void bacnet_command_trace_response(const uint8_t *peer, size_t peer_length,
    const uint8_t *apdu, uint16_t apdu_length, uint64_t uptime_ms, int send_result);
void bacnet_command_trace_snapshot(bacnet_command_trace_snapshot_t *snapshot);
