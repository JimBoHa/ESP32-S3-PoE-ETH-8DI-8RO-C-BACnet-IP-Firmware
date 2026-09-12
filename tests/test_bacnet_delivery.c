/* SPDX-License-Identifier: 0BSD */
/* Isolated protocol tests: real pinned BACnet COV/TSM/BI/BO code, no sockets. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bacnet/bacapp.h"
#include "bacnet/bacdcode.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/service/h_cov.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/cov.h"
#include "bacnet/npdu.h"
#include "bacnet_cov_recovery.h"

static unsigned now_ms;
static unsigned notifications;
static unsigned last_notification_ms;
static uint8_t last_invoke;
static unsigned last_value;
static unsigned relay_callbacks;
static BACNET_BINARY_PV relay_value;
static bool recovery_enabled;
static bool ack_healthy;
static unsigned subscription_lifetime = 28800;
static unsigned healthy_notifications;
static unsigned first_healthy_ms;
static BACNET_ADDRESS subscriber = {.mac_len = 6,
    .mac = {192, 0, 2, 10, 0xba, 0xc0}};

uint32_t Device_Object_Instance_Number(void) { return 1234; }
void Device_Inc_Database_Revision(void) {}

bool Device_Valid_Object_Id(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    return type == OBJECT_BINARY_INPUT && Binary_Input_Valid_Instance(instance);
}

bool Device_Value_List_Supported(BACNET_OBJECT_TYPE type)
{
    return type == OBJECT_BINARY_INPUT;
}

bool Device_COV(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    assert(type == OBJECT_BINARY_INPUT);
    return Binary_Input_Change_Of_Value(instance) || (recovery_enabled &&
        bacnet_cov_recovery_pending(type, instance));
}

void Device_COV_Clear(BACNET_OBJECT_TYPE type, uint32_t instance)
{
    assert(type == OBJECT_BINARY_INPUT);
    Binary_Input_Change_Of_Value_Clear(instance);
    if (recovery_enabled) bacnet_cov_recovery_clear(type, instance);
}

bool Device_Encode_Value_List(BACNET_OBJECT_TYPE type, uint32_t instance,
    BACNET_PROPERTY_VALUE *values)
{
    assert(type == OBJECT_BINARY_INPUT);
    /* Firmware intentionally uses the polarity-aware logical getter. */
    return cov_value_list_encode_enumerated(values,
        Binary_Input_Present_Value(instance), false,
        Binary_Input_Reliability(instance) != RELIABILITY_NO_FAULT_DETECTED,
        false, Binary_Input_Out_Of_Service(instance));
}

void bip_get_my_address(BACNET_ADDRESS *address)
{
    memset(address, 0, sizeof(*address));
    address->mac_len = 6;
    memcpy(address->mac, (uint8_t[]){192, 0, 2, 20, 0xba, 0xc0}, 6);
}

int bip_send_pdu(BACNET_ADDRESS *destination, BACNET_NPDU_DATA *npdu_data,
    uint8_t *pdu, uint16_t length)
{
    (void)destination;
    (void)npdu_data;
    BACNET_ADDRESS dest = {0}, src = {0};
    BACNET_NPDU_DATA decoded = {0};
    int offset = bacnet_npdu_decode(pdu, length, &dest, &src, &decoded);
    assert(offset > 0 && offset < length);
    if (pdu[offset] == PDU_TYPE_CONFIRMED_SERVICE_REQUEST &&
        pdu[offset + 3] == SERVICE_CONFIRMED_COV_NOTIFICATION) {
        BACNET_COV_DATA cov = {0};
        BACNET_PROPERTY_VALUE values[2] = {0};
        bacapp_property_value_list_init(values, 2);
        cov.listOfValues = values;
        int used = cov_notify_decode_service_request(pdu + offset + 4,
            length - offset - 4, &cov);
        assert(used > 0);
        assert(values[0].propertyIdentifier == PROP_PRESENT_VALUE);
        assert(values[0].value.tag == BACNET_APPLICATION_TAG_ENUMERATED);
        last_value = values[0].value.type.Enumerated;
        last_invoke = pdu[offset + 2];
        last_notification_ms = now_ms;
        notifications++;
        if (ack_healthy && cov.monitoredObjectIdentifier.instance >= 9) {
            if (!healthy_notifications) first_healthy_ms = now_ms;
            healthy_notifications++;
            tsm_free_invoke_id(last_invoke);
        }
    }
    return length;
}

static void pump(void)
{
    unsigned calls = 0;
    while (!handler_cov_fsm()) {
        assert(++calls < 1000);
    }
}

static void acknowledge(uint8_t invoke)
{
    uint8_t ack[] = {PDU_TYPE_SIMPLE_ACK, invoke, SERVICE_CONFIRMED_COV_NOTIFICATION};
    /* Use the real dispatcher, with no registered custom COV ACK callback. */
    apdu_handler(&subscriber, ack, sizeof(ack));
    assert(tsm_invoke_id_free(invoke));
}

static void tick(unsigned duration_ms)
{
    for (unsigned elapsed = 0; elapsed < duration_ms; elapsed += 20) {
        now_ms += 20;
        if (recovery_enabled) bacnet_cov_recovery_timer_milliseconds(20);
        tsm_timer_milliseconds(20);
        if (now_ms % 1000 == 0) {
            handler_cov_timer_seconds(1);
        }
        pump();
    }
}

static void subscribe(unsigned process, unsigned instance, bool cancel)
{
    BACNET_SUBSCRIBE_COV_DATA data = {0};
    uint8_t request[128];
    BACNET_CONFIRMED_SERVICE_DATA service = {.invoke_id = 90};
    data.subscriberProcessIdentifier = process;
    data.monitoredObjectIdentifier.type = OBJECT_BINARY_INPUT;
    data.monitoredObjectIdentifier.instance = instance;
    data.issueConfirmedNotifications = true;
    data.lifetime = subscription_lifetime;
    data.cancellationRequest = cancel;
    int length = cov_subscribe_service_request_encode(request, sizeof(request), &data);
    assert(length > 0);
    handler_cov_subscribe(request, length, &subscriber, &service);
}

static void setup(void)
{
    assert(Binary_Input_Create(1) == 1);
    assert(Binary_Input_Present_Value_Set(1, BINARY_INACTIVE));
    handler_cov_init();
    subscribe(16, 1, false);
    pump();
    assert(notifications == 1 && last_value == BINARY_INACTIVE);
    acknowledge(last_invoke);
    pump();
}

static void lost_change(void)
{
    setup();
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    pump();
    assert(notifications == 2 && last_value == BINARY_ACTIVE);
    /* Drop the notification and every retry, while its subscription stays live. */
    tick(12000);
    unsigned after_timeout = notifications;
    unsigned final_retry = last_notification_ms;
    if (recovery_enabled) {
        tick(1000);
        assert(notifications == after_timeout + 1 && last_value == BINARY_ACTIVE);
        assert(last_notification_ms == 13000);
        acknowledge(last_invoke);
        pump();
        tick(180000);
        assert(notifications == after_timeout + 1);
        bacnet_cov_recovery_stats_t stats;
        bacnet_cov_recovery_stats(&stats);
        assert(stats.confirmed_timeouts == 1 && stats.refresh_requests == 1);
        assert(stats.pending_objects == 0 && stats.capacity_errors == 0);
        puts("PASS: lost final edge repaired at 13000 ms; ACK stops recovery; no repeats in next 180 seconds");
        return;
    }
    tick(180000);
    assert(notifications == after_timeout);
    assert(Binary_Input_Present_Value(1) == BINARY_ACTIVE);
    assert(notifications == 5 && final_retry == 9000);
    printf("CHARACTERIZED: lost final edge: 4 sends, last retry at %u ms; "
        "no repair during following 180 seconds; direct value remains Active\n",
        final_retry);
    subscribe(16, 1, false);
    pump();
    assert(notifications == after_timeout + 1 && last_value == BINARY_ACTIVE);
    acknowledge(last_invoke);
    subscribe(16, 1, true);
}

static void change_while_pending(void)
{
    setup();
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    pump();
    uint8_t old_invoke = last_invoke;
    assert(Binary_Input_Present_Value_Set(1, BINARY_INACTIVE));
    pump();
    assert(notifications == 2);
    tick(12000);
    assert(notifications == 6 && last_value == BINARY_INACTIVE);
    assert(last_invoke != old_invoke && last_notification_ms == 12000);
    acknowledge(last_invoke);
    pump();
    puts("PASS: latest edge retained behind unacknowledged COV; delivered at 12000 ms");
}

static void polarity(void)
{
    setup();
    (void)Binary_Input_Polarity_Set(1, POLARITY_REVERSE);
    assert(Binary_Input_Polarity(1) == POLARITY_REVERSE);
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    subscribe(16, 1, false);
    pump();
    assert(last_value == BINARY_ACTIVE);
    acknowledge(last_invoke);
    pump();
    assert(Binary_Input_Present_Value_Set(1, BINARY_INACTIVE));
    pump();
    assert(last_value == BINARY_INACTIVE);
    acknowledge(last_invoke);
    puts("PASS: reverse-polarity logical values agree on subscription and both edges");
}

static void relay_callback(uint32_t instance, BACNET_BINARY_PV old_value,
    BACNET_BINARY_PV value)
{
    (void)old_value;
    assert(instance == 1);
    relay_callbacks++;
    relay_value = value;
}

static void commands(void)
{
    assert(Binary_Output_Create(1) == 1);
    Binary_Output_Write_Present_Value_Callback_Set(relay_callback);
    BACNET_WRITE_PROPERTY_DATA wp = {.object_type = OBJECT_BINARY_OUTPUT,
        .object_instance = 1, .object_property = PROP_PRESENT_VALUE,
        .array_index = BACNET_ARRAY_ALL, .priority = 16};
    uint8_t *value = wp.application_data;
    wp.application_data_len = encode_application_enumerated(value, BINARY_ACTIVE);
    assert(Binary_Output_Write_Property(&wp));
    assert(relay_value == BINARY_ACTIVE && relay_callbacks == 1);
    assert(Binary_Output_Present_Value(1) == BINARY_ACTIVE);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value_Priority(1) == 16);
    wp.application_data_len = encode_application_real(value, 1.0f);
    assert(!Binary_Output_Write_Property(&wp));
    assert(wp.error_code == ERROR_CODE_INVALID_DATA_TYPE);
    assert(Binary_Output_Present_Value(1) == BINARY_ACTIVE);
    wp.application_data_len = encode_application_null(value);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value(1) == BINARY_INACTIVE);
    assert(Binary_Output_Present_Value_Priority(1) == 0);
    puts("PASS: enumerated On accepted; numeric REAL rejected without mutation; NULL releases to Off");
    wp.application_data_len = encode_application_enumerated(value, BINARY_ACTIVE);
    assert(Binary_Output_Write_Property(&wp));
    Binary_Output_Cleanup();
    assert(Binary_Output_Create(1) == 1);
    assert(Binary_Output_Present_Value(1) == BINARY_INACTIVE);
    assert(Binary_Output_Present_Value_Priority(1) == 0);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value(1) == BINARY_ACTIVE);
    puts("PASS: object recreation loses volatile command; fresh command restores current demand");
    wp.priority = 8;
    wp.application_data_len = encode_application_enumerated(value, BINARY_INACTIVE);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value(1) == BINARY_INACTIVE);
    wp.priority = 16;
    wp.application_data_len = encode_application_enumerated(value, BINARY_ACTIVE);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value(1) == BINARY_INACTIVE);
    wp.priority = 8;
    wp.application_data_len = encode_application_null(value);
    assert(Binary_Output_Write_Property(&wp));
    assert(Binary_Output_Present_Value(1) == BINARY_ACTIVE);
    puts("PASS: higher-priority Off wins over On; relinquish returns control to priority 16");
}

static void healthy_edges(void)
{
    setup();
    for (unsigned i = 0; i < 1000; ++i) {
        unsigned expected = (i + 1) % 2;
        assert(Binary_Input_Present_Value_Set(1, expected));
        unsigned before = notifications;
        pump();
        assert(notifications == before + 1 && last_value == expected);
        acknowledge(last_invoke);
        tick(20);
    }
    bacnet_cov_recovery_stats_t stats;
    bacnet_cov_recovery_stats(&stats);
    assert(stats.confirmed_timeouts == 0 && stats.refresh_requests == 0);
    puts("PASS: 1000 healthy edges sent in the next FSM pass, no timeout or extra refresh");
}

static void starvation(void)
{
    handler_cov_init();
    ack_healthy = true;
    for (unsigned i = 1; i <= 12; ++i) {
        assert(Binary_Input_Create(i) == i);
        subscribe(16, i, false);
    }
    pump();
    assert(notifications == 8 && healthy_notifications == 0);
    tick(12000);
    assert(healthy_notifications == 4 && first_healthy_ms == 12000);
    /* Eight failed subscribers must not continually recapture all TSM slots. */
    tick(60000);
    assert(Binary_Input_Present_Value_Set(12, BINARY_ACTIVE));
    unsigned before = healthy_notifications;
    pump();
    tick(14000);
    assert(healthy_notifications > before);
    puts("PASS: 8 unacknowledged subscriptions do not permanently starve 4 healthy subscriptions");
}

static void cancel_recovery(void)
{
    setup();
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    pump();
    tick(12000);
    unsigned before = notifications;
    subscribe(16, 1, true);
    tick(180000);
    assert(notifications == before);
    puts("PASS: cancellation prevents further notifications, including timeout recovery");
}

static void expiry(void)
{
    subscription_lifetime = 12;
    setup();
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    pump();
    tick(12000);
    unsigned before = notifications;
    tick(180000);
    assert(notifications == before);
    uint8_t subscriptions[MAX_PDU];
    assert(handler_cov_encode_subscriptions(subscriptions, sizeof(subscriptions)) == 0);
    puts("PASS: expired subscriptions are not revived by recovery");
}

static void superseded(void)
{
    setup();
    assert(Binary_Input_Present_Value_Set(1, BINARY_ACTIVE));
    pump();
    tick(12500);
    unsigned before = notifications;
    assert(Binary_Input_Present_Value_Set(1, BINARY_INACTIVE));
    pump();
    assert(notifications == before + 1 && last_value == BINARY_INACTIVE);
    acknowledge(last_invoke);
    tick(30000);
    assert(notifications == before + 1 && last_value == BINARY_INACTIVE);
    puts("PASS: a fresh change supersedes pending recovery; stale Active is never replayed");
}

static void ignore_write_timeout(void)
{
    uint8_t invoke = tsm_next_free_invokeID();
    assert(invoke);
    /* Timeout callback must never retry/replay a WriteProperty command. */
    uint8_t pdu[] = {1, 4, 0, 5, invoke, SERVICE_CONFIRMED_WRITE_PROPERTY};
    BACNET_NPDU_DATA npdu = {0};
    tsm_set_confirmed_unsegmented_transaction(invoke, &subscriber, &npdu, pdu, sizeof(pdu));
    tick(13000);
    bacnet_cov_recovery_stats_t stats;
    bacnet_cov_recovery_stats(&stats);
    assert(stats.confirmed_timeouts == 0 && stats.pending_objects == 0);
    assert(notifications == 0);
    tsm_free_invoke_id(invoke);
    puts("PASS: non-COV transaction timeout cannot schedule value refresh or relay command");
}

int main(int argc, char **argv)
{
    assert(argc == 2 || argc == 3);
    apdu_timeout_set(3000);
    apdu_retries_set(3);
    recovery_enabled = argc == 3 && strcmp(argv[2], "recovery") == 0;
    if (recovery_enabled) bacnet_cov_recovery_init();
    if (strcmp(argv[1], "lost-change") == 0) lost_change();
    else if (strcmp(argv[1], "pending-change") == 0) change_while_pending();
    else if (strcmp(argv[1], "polarity") == 0) polarity();
    else if (strcmp(argv[1], "commands") == 0) commands();
    else if (strcmp(argv[1], "healthy") == 0) healthy_edges();
    else if (strcmp(argv[1], "starvation") == 0) starvation();
    else if (strcmp(argv[1], "cancel") == 0) cancel_recovery();
    else if (strcmp(argv[1], "expiry") == 0) expiry();
    else if (strcmp(argv[1], "superseded") == 0) superseded();
    else if (strcmp(argv[1], "ignore-write") == 0) ignore_write_timeout();
    else return 2;
    return 0;
}
