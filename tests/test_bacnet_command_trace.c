/* SPDX-License-Identifier: 0BSD */
#include "bacnet_command_trace.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t peer[] = {192, 0, 2, 10, 0xba, 0xc0};
static const uint8_t other[] = {192, 0, 2, 11, 0xba, 0xc0};
static const uint8_t write[] = {0x00, 0x05, 7, 15, 0x0c, 0, 0x40, 0, 1};
static const uint8_t ack[] = {0x20, 7, 15};

static bacnet_command_trace_snapshot_t snapshot(void)
{
    bacnet_command_trace_snapshot_t result;
    bacnet_command_trace_snapshot(&result);
    return result;
}

static void test_request_response(void)
{
    bacnet_command_trace_reset();
    bacnet_command_trace_begin(peer, sizeof(peer), write, sizeof(write), 100, true);
    bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), 101, 9);
    bacnet_command_trace_end();
    bacnet_command_trace_snapshot_t s = snapshot();
    assert(s.write_property_requests == 1 && s.write_property_multiple_requests == 0);
    assert(s.total_records == 2 && s.retained_records == 2);
    assert(s.records[0].sequence == 1 && s.records[1].sequence == 2);
    assert(s.records[0].uptime_ms == 100 && !s.records[0].outgoing);
    assert(s.records[1].uptime_ms == 101 && s.records[1].outgoing);
    assert(s.records[1].send_result == 9 && s.records[1].service_choice == 15);
    assert(s.records[0].processing_enabled && s.records[0].captured_length == sizeof(write));
    assert(memcmp(s.records[0].apdu, write, sizeof(write)) == 0);
    assert(memcmp(s.records[1].peer, peer, sizeof(peer)) == 0);
    /* A later response cannot be attributed to the completed request. */
    bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), 102, 9);
    assert(snapshot().total_records == 2);
}

static void test_multiple_and_errors(void)
{
    bacnet_command_trace_reset();
    const uint8_t multiple[] = {0, 5, 8, 16};
    const uint8_t reject[] = {0x60, 8, 9};
    const uint8_t abort[] = {0x71, 8, 4};
    const uint8_t error[] = {0x50, 8, 16, 0x91, 2, 0x91, 40};
    bacnet_command_trace_begin(peer, sizeof(peer), multiple, sizeof(multiple), 200, true);
    bacnet_command_trace_response(peer, sizeof(peer), reject, sizeof(reject), 201, 9);
    bacnet_command_trace_response(peer, sizeof(peer), abort, sizeof(abort), 202, -1);
    bacnet_command_trace_response(peer, sizeof(peer), error, sizeof(error), 203, 13);
    bacnet_command_trace_snapshot_t s = snapshot();
    assert(s.write_property_requests == 0 && s.write_property_multiple_requests == 1);
    assert(s.total_records == 4 && s.records[1].service_choice == 16);
    assert(s.records[2].send_result == -1);
    assert(memcmp(s.records[3].apdu, error, sizeof(error)) == 0);
}

static void test_filtering_and_context(void)
{
    bacnet_command_trace_reset();
    const uint8_t read[] = {0, 5, 7, 12};
    const uint8_t wrong_service[] = {0x20, 7, 16};
    const uint8_t wrong_invoke[] = {0x20, 8, 15};
    const uint8_t request[] = {0x00, 7, 15};
    const uint8_t complex_ack[] = {0x30, 7, 15};
    bacnet_command_trace_begin(peer, sizeof(peer), read, sizeof(read), 0, true);
    assert(snapshot().total_records == 0);
    bacnet_command_trace_begin(peer, sizeof(peer), write, sizeof(write), 0, true);
    bacnet_command_trace_response(other, sizeof(other), ack, sizeof(ack), 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), wrong_service, sizeof(wrong_service), 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), wrong_invoke, sizeof(wrong_invoke), 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), request, sizeof(request), 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), complex_ack, sizeof(complex_ack), 0, 9);
    assert(snapshot().total_records == 1);
    /* Reuse of an invoke ID by a read must not inherit write attribution. */
    bacnet_command_trace_begin(peer, sizeof(peer), read, sizeof(read), 0, true);
    bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), 0, 9);
    assert(snapshot().total_records == 1);
    bacnet_command_trace_begin(peer, sizeof(peer), write, sizeof(write), 0, false);
    bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), 0, 9);
    assert(snapshot().total_records == 2 && !snapshot().records[1].processing_enabled);
}

static void test_bounds_and_segmentation(void)
{
    bacnet_command_trace_reset();
    const uint8_t segmented[] = {0x08, 5, 9, 0, 1, 15};
    for (unsigned length = 0; length < sizeof(segmented); ++length) {
        bacnet_command_trace_begin(peer, sizeof(peer), segmented, length, 0, true);
    }
    bacnet_command_trace_begin(NULL, sizeof(peer), write, sizeof(write), 0, true);
    bacnet_command_trace_begin(peer, 5, write, sizeof(write), 0, true);
    bacnet_command_trace_begin(peer, sizeof(peer), NULL, 4, 0, true);
    assert(snapshot().total_records == 0);
    bacnet_command_trace_begin(peer, sizeof(peer), segmented, sizeof(segmented), 0, true);
    bacnet_command_trace_response(NULL, sizeof(peer), ack, sizeof(ack), 0, 9);
    bacnet_command_trace_response(peer, 5, ack, sizeof(ack), 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), NULL, 4, 0, 9);
    bacnet_command_trace_response(peer, sizeof(peer), ack, 2, 0, 9);
    assert(snapshot().total_records == 1 && snapshot().records[0].service_choice == 15);
    uint8_t long_write[BACNET_COMMAND_TRACE_APDU_BYTES + 40U];
    memset(long_write, 0xa5, sizeof(long_write));
    memcpy(long_write, write, sizeof(write));
    bacnet_command_trace_begin(peer, sizeof(peer), long_write, sizeof(long_write), 0, true);
    bacnet_command_trace_snapshot_t s = snapshot();
    assert(s.records[1].apdu_length == sizeof(long_write));
    assert(s.records[1].captured_length == BACNET_COMMAND_TRACE_APDU_BYTES);
    assert(memcmp(s.records[1].apdu, long_write, BACNET_COMMAND_TRACE_APDU_BYTES) == 0);
    bacnet_command_trace_snapshot(NULL);
}

static void test_ring_and_snapshot_independence(void)
{
    bacnet_command_trace_reset();
    for (unsigned i = 0; i < 10; ++i) {
        bacnet_command_trace_begin(peer, sizeof(peer), write, sizeof(write), i * 2U, true);
        bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), i * 2U + 1U, 9);
        bacnet_command_trace_end();
    }
    bacnet_command_trace_snapshot_t s = snapshot();
    assert(s.total_records == 20 && s.retained_records == BACNET_COMMAND_TRACE_CAPACITY);
    assert(s.write_property_requests == 10);
    for (unsigned i = 0; i < s.retained_records; ++i) {
        assert(s.records[i].sequence == 13U + i);
        assert(s.records[i].uptime_ms == 12U + i);
    }
    s.records[0].apdu[0] = 0xff;
    assert(snapshot().records[0].apdu[0] == 0);
    bacnet_command_trace_reset();
    assert(snapshot().total_records == 0 && snapshot().retained_records == 0);
    bacnet_command_trace_response(peer, sizeof(peer), ack, sizeof(ack), 0, 9);
    assert(snapshot().total_records == 0);
}

int main(void)
{
    test_request_response();
    test_multiple_and_errors();
    test_filtering_and_context();
    test_bounds_and_segmentation();
    test_ring_and_snapshot_independence();
    puts("BACnet command trace tests passed");
    return 0;
}
