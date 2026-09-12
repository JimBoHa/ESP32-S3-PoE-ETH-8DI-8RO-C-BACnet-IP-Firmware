/* SPDX-License-Identifier: 0BSD */
#include "bacnet_app.h"

#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"

#include "bacnet/apdu.h"
#include "bacnet/bacdcode.h"
#include "bacnet/bacstr.h"
#include "bacnet/basic/binding/address.h"
#include "bacnet/basic/npdu/h_npdu.h"
#include "bacnet/basic/object/ai.h"
#include "bacnet/basic/object/bi.h"
#include "bacnet/basic/object/bo.h"
#include "bacnet/basic/object/bv.h"
#include "bacnet/basic/object/csv.h"
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/netport.h"
#include "bacnet/basic/service/h_apdu.h"
#include "bacnet/basic/service/h_cov.h"
#include "bacnet/basic/service/h_noserv.h"
#include "bacnet/basic/service/h_rp.h"
#include "bacnet/basic/service/h_rpm.h"
#include "bacnet/basic/service/h_whohas.h"
#include "bacnet/basic/service/h_whois.h"
#include "bacnet/basic/service/h_wp.h"
#include "bacnet/basic/service/s_iam.h"
#include "bacnet/basic/service/s_whois.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/cov.h"
#include "bacnet/dcc.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/npdu.h"
#include "bacnet/iam.h"

#include "bip_esp32.h"
#include "bacnet_instance.h"
#include "bacnet_cov_recovery.h"
#include "board_io.h"
#include "config_store.h"
#include "clock_service.h"
#include "ethernet_manager.h"
#include "firmware.h"

#define STATUS_BI_ETHERNET_LINK 1001U
#define STATUS_BI_IPV4_ASSIGNED 1002U
#define STATUS_BI_RELAY_CONTROLLER 1003U
#define STATUS_BI_RTC_PRESENT 1004U
#define STATUS_AI_UPTIME_SECONDS 1001U
#define STATUS_AI_FREE_HEAP_BYTES 1002U
#define STATUS_AI_MIN_HEAP_BYTES 1003U
#define STATUS_AI_REBOOT_COUNT 1004U
#define CONFIG_CSV_HOSTNAME 1U
#define CONFIG_BV_RELAY_RESTORE 1U

static const char *TAG = "bacnet";
static firmware_config_t s_config;
static volatile bool s_running;
static bool s_startup_complete;
static bacnet_announcement_t s_announcement;
static bacnet_restart_clock_t s_restart_clock;
static const char *s_restart_clock_source = "pending";
static clock_service_snapshot_t s_clock_snapshot;
static bool s_clock_snapshot_valid;
static bool s_clock_poll_started;
static uint64_t s_clock_last_poll_ms;
static int32_t s_device_optional_properties[64];
static volatile uint32_t s_packet_count;
static bacnet_instance_t s_instance;
static volatile uint32_t s_active_instance;
static const char *volatile s_instance_status = "waiting-for-network";
static volatile uint32_t s_instance_conflicts;
static uint8_t s_pdu_buffer[BIP_MPDU_MAX];
static SemaphoreHandle_t s_start_signal;
static SemaphoreHandle_t s_object_mutex;
static esp_err_t s_start_result;
/* bacnet-stack object metadata setters retain these pointers for zero-copy
   reads, so generated strings must live for the lifetime of the application. */
static char s_serial_number[18];
static char s_input_descriptions[FW_DI_COUNT][64];
static char s_output_descriptions[FW_RELAY_COUNT][80];
static BACNET_RESTART_REASON restart_reason_to_bacnet(void);

static bool clock_datetime(const struct tm *local, int64_t unix_us,
    BACNET_DATE_TIME *datetime)
{
    if (!local || !datetime || local->tm_year < 0 || local->tm_year > 254 ||
        local->tm_mon < 0 || local->tm_mon > 11 ||
        local->tm_mday < 1 || local->tm_mday > 31 ||
        local->tm_hour < 0 || local->tm_hour > 23 ||
        local->tm_min < 0 || local->tm_min > 59 ||
        local->tm_sec < 0 || local->tm_sec > 59 || unix_us < 0) {
        return false;
    }
    datetime_set_date(&datetime->date, local->tm_year + 1900,
        local->tm_mon + 1, local->tm_mday);
    datetime_set_time(&datetime->time, local->tm_hour, local->tm_min,
        local->tm_sec, (unix_us % 1000000) / 10000);
    return datetime_is_valid(&datetime->date, &datetime->time) &&
        !datetime_wildcard_present(datetime);
}

/* Called only under the BACnet object mutex, never from an NTP/TCP-IP callback.
   Seed the upstream advancing clock, with exact offset/DST reads below. */
static void update_bacnet_clock(uint64_t now_ms)
{
    if (s_clock_poll_started && now_ms - s_clock_last_poll_ms < 1000U) {
        return;
    }
    s_clock_poll_started = true;
    s_clock_last_poll_ms = now_ms;
    clock_service_snapshot_t snapshot;
    BACNET_DATE_TIME local, boot;
    if (!clock_service_snapshot_get(&snapshot)) {
        return; /* A busy snapshot does not invalidate the advancing clock. */
    }
    if (!snapshot.valid ||
        !clock_datetime(&snapshot.local_time, snapshot.utc_unix_us, &local) ||
        !clock_datetime(&snapshot.boot_local_time, snapshot.boot_utc_unix_us, &boot)) {
        s_clock_snapshot_valid = false;
        return;
    }
    s_clock_snapshot = snapshot;
    s_clock_snapshot_valid = true;
    datetime_timesync(&local.date, &local.time, false);
    (void)datetime_utc_offset_minutes_set(snapshot.utc_offset_minutes);
}

static BACNET_TIMESTAMP fallback_restart_timestamp(void)
{
    BACNET_DATE_TIME local;
    BACNET_TIMESTAMP timestamp;
    datetime_set_date(&local.date, 1990, 1, 1);
    datetime_set_time(&local.time, 0, 0, 0, 0);
    bacapp_timestamp_datetime_set(&timestamp, &local);
    return timestamp;
}

static void select_restart_timestamp(uint64_t now_ms, bool ready)
{
    if (ready && !s_restart_clock.selected && s_restart_clock.wait_started &&
        now_ms - s_restart_clock.wait_started_ms >= BACNET_RESTART_CLOCK_WAIT_MS) {
        /* A reply just before the deadline must not be missed because the
           ordinary local-clock refresh is rate limited to once per second. */
        s_clock_poll_started = false;
        update_bacnet_clock(now_ms);
    }
    BACNET_DATE_TIME boot;
    bool valid = s_clock_snapshot_valid &&
        clock_datetime(&s_clock_snapshot.boot_local_time,
            s_clock_snapshot.boot_utc_unix_us, &boot);
    bacnet_restart_clock_choice_t choice = bacnet_restart_clock_select(
        &s_restart_clock, now_ms, ready, valid);
    if (choice == BACNET_RESTART_CLOCK_WAIT) {
        return;
    }
    BACNET_TIMESTAMP timestamp = fallback_restart_timestamp();
    if (choice == BACNET_RESTART_CLOCK_VALID) {
        bacapp_timestamp_datetime_set(&timestamp, &boot);
    }
    if (bacnet_restart_set_timestamp_before_ready(&timestamp)) {
        (void)Device_Set_Time_Of_Restart(&timestamp);
        s_restart_clock_source = choice == BACNET_RESTART_CLOCK_VALID ?
            s_clock_snapshot.source : "unsynchronized-1990-fallback";
    } else {
        ESP_LOGE(TAG, "Restart timestamp selection rejected; notification suppressed");
    }
}

/* The upstream millisecond clock computes DST from its own rules. Return the
   validated system timezone's actual offset/DST instead, while reading its
   seeded advancing local clock. No additional Device writes are enabled. */
static int clock_read_property(BACNET_READ_PROPERTY_DATA *data)
{
    BACNET_DATE_TIME local;
    uint8_t encoded[8];
    int length;
    (void)datetime_local(&local.date, &local.time, NULL, NULL);
    switch (data->object_property) {
        case PROP_LOCAL_DATE:
            length = encode_application_date(encoded, &local.date);
            break;
        case PROP_LOCAL_TIME:
            length = encode_application_time(encoded, &local.time);
            break;
        case PROP_UTC_OFFSET:
            length = encode_application_signed(encoded, s_clock_snapshot.utc_offset_minutes);
            break;
        default: /* PROP_DAYLIGHT_SAVINGS_STATUS */
            length = encode_application_boolean(encoded, s_clock_snapshot.daylight_saving);
            break;
    }
    data->error_class = ERROR_CLASS_PROPERTY;
    if (data->array_index != BACNET_ARRAY_ALL) {
        data->error_code = ERROR_CODE_PROPERTY_IS_NOT_AN_ARRAY;
        return BACNET_STATUS_ERROR;
    }
    if (!data->application_data || data->application_data_len < length) {
        data->error_code = ERROR_CODE_ABORT_SEGMENTATION_NOT_SUPPORTED;
        return BACNET_STATUS_ABORT;
    }
    memcpy(data->application_data, encoded, (size_t)length);
    return length;
}

static bool read_only_write_property(BACNET_WRITE_PROPERTY_DATA *data)
{
    if (data) {
        data->error_class = ERROR_CLASS_PROPERTY;
        data->error_code = ERROR_CODE_WRITE_ACCESS_DENIED;
    }
    return false;
}

static void read_only_writable_property_list(
    uint32_t object_instance, const int32_t **properties)
{
    static const int32_t writable_properties[] = {-1};
    (void)object_instance;
    if (properties) {
        *properties = writable_properties;
    }
}

static bool binary_input_encode_value_list(
    uint32_t object_instance, BACNET_PROPERTY_VALUE *value_list)
{
    bool fault = Binary_Input_Reliability(object_instance) !=
        RELIABILITY_NO_FAULT_DETECTED;
    return cov_value_list_encode_enumerated(value_list,
        Binary_Input_Present_Value(object_instance), false, fault, false,
        Binary_Input_Out_Of_Service(object_instance));
}

/* After a confirmed COV times out, request fresh current-value encoding through
   the normal subscription FSM. Never replay the failed packet's old value,
   create a subscription, extend a lifetime, or write a point. A real change
   may supersede a scheduled refresh before its backoff expires. */
#define COV_RECOVERY_WRAPPERS(name, object_type, upstream) \
    static bool name##_cov_changed(uint32_t instance) \
    { \
        return upstream##_Change_Of_Value(instance) || \
            bacnet_cov_recovery_pending(object_type, instance); \
    } \
    static void name##_cov_clear(uint32_t instance) \
    { \
        upstream##_Change_Of_Value_Clear(instance); \
        bacnet_cov_recovery_clear(object_type, instance); \
    }

COV_RECOVERY_WRAPPERS(input, OBJECT_BINARY_INPUT, Binary_Input)
COV_RECOVERY_WRAPPERS(output, OBJECT_BINARY_OUTPUT, Binary_Output)
COV_RECOVERY_WRAPPERS(analog, OBJECT_ANALOG_INPUT, Analog_Input)
COV_RECOVERY_WRAPPERS(binary_value, OBJECT_BINARY_VALUE, Binary_Value)
COV_RECOVERY_WRAPPERS(string_value, OBJECT_CHARACTERSTRING_VALUE, CharacterString_Value)
#undef COV_RECOVERY_WRAPPERS

static bool binary_output_write_property(BACNET_WRITE_PROPERTY_DATA *data)
{
    if (!data) {
        return false;
    }
    if (data->object_property != PROP_PRESENT_VALUE) {
        data->error_class = ERROR_CLASS_PROPERTY;
        data->error_code = ERROR_CODE_WRITE_ACCESS_DENIED;
        return false;
    }
    return Binary_Output_Write_Property(data);
}

static void binary_output_writable_property_list(
    uint32_t object_instance, const int32_t **properties)
{
    static const int32_t writable_properties[] = {PROP_PRESENT_VALUE, -1};
    (void)object_instance;
    if (properties) {
        *properties = writable_properties;
    }
}

static int device_read_property(BACNET_READ_PROPERTY_DATA *data)
{
    if (data && data->object_property == PROP_RESTART_NOTIFICATION_RECIPIENTS) {
        return bacnet_restart_read_property(data);
    }
    if (data && s_clock_snapshot_valid &&
        (data->object_property == PROP_LOCAL_DATE ||
         data->object_property == PROP_LOCAL_TIME ||
         data->object_property == PROP_UTC_OFFSET ||
         data->object_property == PROP_DAYLIGHT_SAVINGS_STATUS)) {
        return clock_read_property(data);
    }
    return Device_Read_Property_Local(data);
}

static bool device_write_property(BACNET_WRITE_PROPERTY_DATA *data)
{
    if (!data) {
        return false;
    }
    if (data->object_property == PROP_RESTART_NOTIFICATION_RECIPIENTS) {
        return bacnet_restart_write_property(data);
    }
    data->error_class = ERROR_CLASS_PROPERTY;
    data->error_code = Device_Objects_Property_List_Member(OBJECT_DEVICE,
        data->object_instance, data->object_property) ?
        ERROR_CODE_WRITE_ACCESS_DENIED : ERROR_CODE_UNKNOWN_PROPERTY;
    return false;
}

static void device_property_lists(const int32_t **required,
    const int32_t **optional, const int32_t **proprietary)
{
    Device_Property_Lists(required, NULL, proprietary);
    if (optional) {
        *optional = s_device_optional_properties;
    }
}

static bool initialize_device_property_list(void)
{
    const int32_t *optional = NULL;
    Device_Property_Lists(NULL, &optional, NULL);
    unsigned count = 0;
    bool present = false;
    while (optional && optional[count] != -1) {
        if (count >= sizeof(s_device_optional_properties) /
                sizeof(s_device_optional_properties[0]) - 2U) {
            return false;
        }
        present |= optional[count] == PROP_RESTART_NOTIFICATION_RECIPIENTS;
        s_device_optional_properties[count] = optional[count];
        ++count;
    }
    if (!present) {
        s_device_optional_properties[count++] = PROP_RESTART_NOTIFICATION_RECIPIENTS;
    }
    s_device_optional_properties[count] = -1;
    return true;
}

static bool restart_persist(const uint8_t *bytes, size_t length, void *context)
{
    (void)context;
    return config_store_restart_recipients_set(bytes, length) == ESP_OK;
}

static bool restart_resolve(const BACNET_RECIPIENT *recipient,
    BACNET_ADDRESS *destination, void *context)
{
    (void)context;
    memset(destination, 0, sizeof(*destination));
    if (recipient->tag == BACNET_RECIPIENT_TAG_DEVICE) {
        unsigned max_apdu = 0;
        return address_bind_request(recipient->type.device.instance,
                   &max_apdu, destination) && destination->mac_len == 6 &&
            destination->net != BACNET_BROADCAST_NETWORK;
    }
    if (recipient->tag != BACNET_RECIPIENT_TAG_ADDRESS ||
        recipient->type.address.net != 0) {
        return false;
    }
    *destination = recipient->type.address;
    /* net=0/mac_len=0 is the canonical local broadcast. Keep it local;
       bip_get_broadcast_address() instead constructs a global NPDU broadcast. */
    return destination->mac_len == 0 || destination->mac_len == 6;
}

static void restart_discover(const BACNET_RECIPIENT *recipient, void *context)
{
    (void)context;
    if (recipient->tag == BACNET_RECIPIENT_TAG_DEVICE) {
        Send_WhoIs_Local(recipient->type.device.instance,
            recipient->type.device.instance);
    }
}

static int restart_send(const BACNET_ADDRESS *destination, const uint8_t *apdu,
    size_t length, void *context)
{
    (void)context;
    /* Never pass an unresolved routed address to bip_send_pdu: mac_len=0
       would turn it into a broadcast. No destination-rewriting UCOV helper. */
    if (!destination || !apdu ||
        (destination->net != 0 && (destination->mac_len != 6 ||
            destination->net == BACNET_BROADCAST_NETWORK))) {
        return -1;
    }
    uint8_t pdu[MAX_PDU];
    BACNET_ADDRESS target = *destination, source = {0};
    BACNET_NPDU_DATA npdu;
    bip_get_my_address(&source);
    npdu_encode_npdu_data(&npdu, false, MESSAGE_PRIORITY_NORMAL);
    int offset = npdu_encode_pdu(pdu, &target, &source, &npdu);
    if (offset <= 0 || (size_t)offset > sizeof(pdu) ||
        length > sizeof(pdu) - (size_t)offset) {
        return -1;
    }
    memcpy(pdu + offset, apdu, length);
    return bip_send_pdu(&target, &npdu, pdu, (unsigned)offset + length);
}

static int startup_announcement_send(void *context)
{
    (void)context;
    if (dcc_communication_initiation_disabled()) {
        return -1;
    }
    /* Encode locally so the actual transport result reaches the bounded
       scheduler. This is identity discovery, not a claim of remote receipt. */
    uint8_t apdu[32];
    int length = iam_encode_apdu(apdu, Device_Object_Instance_Number(), MAX_APDU,
        Device_Segmentation_Supported(), Device_Vendor_Identifier());
    if (length <= 0 || (size_t)length > sizeof(apdu)) {
        return -1;
    }
    const BACNET_ADDRESS local_broadcast = {0};
    return restart_send(&local_broadcast, apdu, (size_t)length, NULL);
}

static void initialize_restart_notification(void)
{
    /* Only initialize a fallback when no valid clock exists. The notification
       timestamp stays deferred for a bounded post-readiness clock wait. */
    BACNET_TIMESTAMP fallback = fallback_restart_timestamp();
    update_bacnet_clock((uint64_t)esp_timer_get_time() / 1000U);
    if (!s_clock_snapshot_valid) {
        datetime_timesync(&fallback.value.dateTime.date,
            &fallback.value.dateTime.time, false);
        (void)datetime_utc_offset_minutes_set(0);
        datetime_dst_enabled_set(false);
    }
    (void)Device_Set_Time_Of_Restart(&fallback);

    uint8_t encoded[BACNET_RESTART_RECIPIENT_BYTES_MAX];
    size_t length = 0;
    esp_err_t loaded = config_store_restart_recipients_get(encoded,
        sizeof(encoded), &length);
    bacnet_restart_load_state_t state = loaded == ESP_OK ?
        BACNET_RESTART_LOAD_VALID : loaded == ESP_ERR_NVS_NOT_FOUND ?
        BACNET_RESTART_LOAD_MISSING : BACNET_RESTART_LOAD_INVALID;
    const bacnet_restart_callbacks_t callbacks = {
        .persist = restart_persist, .resolve = restart_resolve,
        .discover = restart_discover, .send = restart_send,
    };
    bacnet_restart_init(state, encoded, length, NULL,
        restart_reason_to_bacnet(), &callbacks, NULL);
}

static object_functions_t s_object_table[] = {
    {OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance,
        Device_Valid_Object_Instance_Number, Device_Object_Name,
        device_read_property, device_write_property,
        device_property_lists, DeviceGetRRInfo, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, bacnet_restart_writable_property_list},
    {OBJECT_BINARY_INPUT, Binary_Input_Init, Binary_Input_Count,
        Binary_Input_Index_To_Instance, Binary_Input_Valid_Instance,
        Binary_Input_Object_Name, Binary_Input_Read_Property, NULL,
        Binary_Input_Property_Lists, NULL, NULL, binary_input_encode_value_list,
        input_cov_changed, input_cov_clear,
        NULL, NULL, NULL, Binary_Input_Create, Binary_Input_Delete, NULL,
        read_only_writable_property_list},
    {OBJECT_ANALOG_INPUT, Analog_Input_Init, Analog_Input_Count,
        Analog_Input_Index_To_Instance, Analog_Input_Valid_Instance,
        Analog_Input_Object_Name, Analog_Input_Read_Property,
        NULL, Analog_Input_Property_Lists, NULL, NULL,
        Analog_Input_Encode_Value_List, analog_cov_changed,
        analog_cov_clear, Analog_Input_Intrinsic_Reporting,
        NULL, NULL, Analog_Input_Create, Analog_Input_Delete, NULL,
        NULL},
    {OBJECT_BINARY_OUTPUT, Binary_Output_Init, Binary_Output_Count,
        Binary_Output_Index_To_Instance, Binary_Output_Valid_Instance,
        Binary_Output_Object_Name, Binary_Output_Read_Property,
        binary_output_write_property, Binary_Output_Property_Lists, NULL, NULL,
        Binary_Output_Encode_Value_List, output_cov_changed,
        output_cov_clear, NULL, NULL, NULL,
        Binary_Output_Create, Binary_Output_Delete, NULL,
        binary_output_writable_property_list},
    {OBJECT_BINARY_VALUE, Binary_Value_Init, Binary_Value_Count,
        Binary_Value_Index_To_Instance, Binary_Value_Valid_Instance,
        Binary_Value_Object_Name, Binary_Value_Read_Property,
        read_only_write_property, Binary_Value_Property_Lists, NULL, NULL,
        Binary_Value_Encode_Value_List, binary_value_cov_changed,
        binary_value_cov_clear, NULL, NULL, NULL, NULL, NULL, NULL,
        read_only_writable_property_list},
    {OBJECT_CHARACTERSTRING_VALUE, CharacterString_Value_Init,
        CharacterString_Value_Count, CharacterString_Value_Index_To_Instance,
        CharacterString_Value_Valid_Instance, CharacterString_Value_Object_Name,
        CharacterString_Value_Read_Property, read_only_write_property,
        CharacterString_Value_Property_Lists, NULL, NULL,
        CharacterString_Value_Encode_Value_List,
        string_value_cov_changed,
        string_value_cov_clear, NULL, NULL, NULL, NULL,
        NULL, NULL, read_only_writable_property_list},
    {OBJECT_NETWORK_PORT, Network_Port_Init, Network_Port_Count,
        Network_Port_Index_To_Instance, Network_Port_Valid_Instance,
        Network_Port_Object_Name, Network_Port_Read_Property, NULL,
        Network_Port_Property_Lists, Network_Port_Read_Range, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {MAX_BACNET_OBJECT_TYPE, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL},
};

unsigned long mstimer_now(void)
{
    return (unsigned long)(esp_timer_get_time() / 1000ULL);
}

static BACNET_RESTART_REASON restart_reason_to_bacnet(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:
        case ESP_RST_BROWNOUT:
            return RESTART_REASON_DETECTED_POWER_LOST;
        case ESP_RST_TASK_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_WDT:
            return RESTART_REASON_HARDWARE_WATCHDOG;
        case ESP_RST_SW:
            return RESTART_REASON_WARMSTART;
        case ESP_RST_PANIC:
            return RESTART_REASON_SOFTWARE_WATCHDOG;
        default:
            return RESTART_REASON_COLDSTART;
    }
}

static esp_err_t relay_effective_value_apply(uint32_t instance,
    BACNET_BINARY_PV value)
{
    if (instance < 1U || instance > FW_RELAY_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = board_io_relay_set(instance - 1U, value == BINARY_ACTIVE);
    BACNET_RELIABILITY reliability = result == ESP_OK ? RELIABILITY_NO_FAULT_DETECTED :
        RELIABILITY_UNRELIABLE_OTHER;
    for (uint32_t output = 1; output <= FW_RELAY_COUNT; ++output) {
        (void)Binary_Output_Reliability_Set(output, reliability);
    }
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Relay %lu command failed: %s", (unsigned long)instance,
            esp_err_to_name(result));
    }
    return result;
}

static void relay_write_callback(uint32_t instance, BACNET_BINARY_PV old_value,
    BACNET_BINARY_PV value)
{
    (void)old_value;
    (void)relay_effective_value_apply(instance, value);
}

static void create_binary_input(uint32_t instance, const char *name, const char *description)
{
    (void)Binary_Input_Create(instance);
    (void)Binary_Input_Name_Set(instance, name);
    (void)Binary_Input_Description_Set(instance, description);
    (void)Binary_Input_Reliability_Set(instance, RELIABILITY_NO_FAULT_DETECTED);
}

static void create_analog_input(uint32_t instance, const char *name,
    const char *description, BACNET_ENGINEERING_UNITS units, float cov_increment)
{
    (void)Analog_Input_Create(instance);
    (void)Analog_Input_Name_Set(instance, name);
    (void)Analog_Input_Description_Set(instance, description);
    (void)Analog_Input_Units_Set(instance, units);
    Analog_Input_COV_Increment_Set(instance, cov_increment);
    (void)Analog_Input_Reliability_Set(instance, RELIABILITY_NO_FAULT_DETECTED);
}

static bool create_configuration_objects(void)
{
    BACNET_CHARACTER_STRING hostname;
    bool valid = CharacterString_Value_Create(CONFIG_CSV_HOSTNAME) ==
        CONFIG_CSV_HOSTNAME;
    valid = CharacterString_Value_Name_Set(CONFIG_CSV_HOSTNAME,
        FW_CONFIG_CSV_HOSTNAME_NAME) && valid;
    valid = CharacterString_Value_Description_Set(CONFIG_CSV_HOSTNAME,
        "Persistent Ethernet hostname; changes require reboot") && valid;
    valid = characterstring_init_ansi(&hostname, s_config.hostname) && valid;
    valid = CharacterString_Value_Present_Value_Set(CONFIG_CSV_HOSTNAME,
        &hostname) && valid;

    valid = Binary_Value_Create(CONFIG_BV_RELAY_RESTORE) ==
        CONFIG_BV_RELAY_RESTORE && valid;
    valid = Binary_Value_Name_Set(CONFIG_BV_RELAY_RESTORE,
        FW_CONFIG_BV_RELAY_RESTORE_NAME) && valid;
    valid = Binary_Value_Description_Set(CONFIG_BV_RELAY_RESTORE,
        "Persistent relay-state restoration policy used at power-up") && valid;
    valid = Binary_Value_Inactive_Text_Set(CONFIG_BV_RELAY_RESTORE,
        "Disabled") && valid;
    valid = Binary_Value_Active_Text_Set(CONFIG_BV_RELAY_RESTORE,
        "Enabled") && valid;
    valid = Binary_Value_Reliability_Set(CONFIG_BV_RELAY_RESTORE,
        RELIABILITY_NO_FAULT_DETECTED) && valid;
    valid = Binary_Value_Present_Value_Set(CONFIG_BV_RELAY_RESTORE,
        s_config.restore_relay_state ? BINARY_ACTIVE : BINARY_INACTIVE) && valid;
    Binary_Value_Write_Disable(CONFIG_BV_RELAY_RESTORE);
    return valid;
}

static bool initialize_objects(void)
{
    if (!initialize_device_property_list()) {
        return false;
    }
    Device_Init(s_object_table);
    (void)Device_Set_Object_Instance_Number(s_config.device_instance);
    (void)Device_Object_Name_ANSI_Init(s_config.device_name);
    (void)Device_Set_Vendor_Name(s_config.vendor_name, strlen(s_config.vendor_name));
    Device_Set_Vendor_Identifier(s_config.vendor_id);
    (void)Device_Set_Model_Name(FW_MODEL_NAME, strlen(FW_MODEL_NAME));
    const esp_app_desc_t *description = esp_app_get_description();
    (void)Device_Set_Firmware_Revision(description->version, strlen(description->version));
    (void)Device_Set_Application_Software_Version(description->version, strlen(description->version));
    (void)Device_Set_Description(FW_PRODUCT_NAME, strlen(FW_PRODUCT_NAME));
    (void)Device_Set_Location(s_config.location, strlen(s_config.location));
    (void)Device_Last_Restart_Reason_Set(restart_reason_to_bacnet());
    (void)Device_Set_System_Status(STATUS_OPERATIONAL, true);
    initialize_restart_notification();

    uint8_t mac[6];
    ethernet_manager_mac_get(mac);
    snprintf(s_serial_number, sizeof(s_serial_number),
        "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    (void)Device_Serial_Number_Set(s_serial_number, strlen(s_serial_number));

    for (uint32_t i = 0; i < FW_DI_COUNT; ++i) {
        bool inverted = (s_config.input_invert_mask & (1U << i)) != 0;
        snprintf(s_input_descriptions[i], sizeof(s_input_descriptions[i]),
            "Opto-isolated digital input DI%lu; configured active-%s",
            (unsigned long)(i + 1U), inverted ? "low" : "high");
        create_binary_input(i + 1U, s_config.input_names[i],
            s_input_descriptions[i]);
        (void)Binary_Input_Polarity_Set(i + 1U,
            inverted ? POLARITY_REVERSE : POLARITY_NORMAL);
        (void)Binary_Input_Inactive_Text_Set(i + 1U, "Inactive");
        (void)Binary_Input_Active_Text_Set(i + 1U, "Active");
    }

    create_binary_input(STATUS_BI_ETHERNET_LINK, FW_STATUS_BI_ETHERNET_NAME,
        "W5500 physical Ethernet link is up");
    create_binary_input(STATUS_BI_IPV4_ASSIGNED, FW_STATUS_BI_IPV4_NAME,
        "Ethernet interface has an IPv4 address");
    create_binary_input(STATUS_BI_RELAY_CONTROLLER, FW_STATUS_BI_RELAY_NAME,
        "TCA9554 relay output controller is responding");
    create_binary_input(STATUS_BI_RTC_PRESENT, FW_STATUS_BI_RTC_NAME,
        "PCF85063 real-time clock is responding");

    for (uint32_t i = 0; i < FW_RELAY_COUNT; ++i) {
        uint32_t instance = i + 1U;
        (void)Binary_Output_Create(instance);
        (void)Binary_Output_Name_Set(instance, s_config.relay_names[i]);
        snprintf(s_output_descriptions[i], sizeof(s_output_descriptions[i]),
            "Relay RO%lu command; state is commanded, not contact feedback",
            (unsigned long)instance);
        (void)Binary_Output_Description_Set(instance,
            s_output_descriptions[i]);
        (void)Binary_Output_Inactive_Text_Set(instance, "Off");
        (void)Binary_Output_Active_Text_Set(instance, "On");
        BACNET_BINARY_PV relinquish = board_io_relay_get(i) ? BINARY_ACTIVE : BINARY_INACTIVE;
        (void)Binary_Output_Relinquish_Default_Set(instance, relinquish);
        (void)Binary_Output_Reliability_Set(instance, RELIABILITY_NO_FAULT_DETECTED);
    }
    Binary_Output_Write_Present_Value_Callback_Set(relay_write_callback);

    (void)Network_Port_Object_Instance_Number_Set(0, 1);
    (void)Network_Port_Name_Set(1, FW_NETWORK_PORT_NAME);
    (void)Network_Port_Description_Set(1, "W5500 BACnet/IP over Ethernet");
    (void)Network_Port_Type_Set(1, PORT_TYPE_BIP);
    (void)Network_Port_Network_Number_Set(1, 0);
    (void)Network_Port_BIP_Port_Set(1, s_config.bacnet_port);
    (void)Network_Port_BIP_Mode_Set(1, BACNET_IP_MODE_NORMAL);
    (void)Network_Port_APDU_Length_Set(1, MAX_APDU);
    (void)Network_Port_Link_Speed_Set(1, 100000000.0F);
    (void)Network_Port_IP_DHCP_Enable_Set(1, s_config.dhcp_enabled);
    (void)Network_Port_Reliability_Set(1, RELIABILITY_NO_FAULT_DETECTED);
    (void)Network_Port_Out_Of_Service_Set(1, false);
    (void)Network_Port_Quality_Set(1, s_config.dhcp_enabled ?
        PORT_QUALITY_LEARNED : PORT_QUALITY_CONFIGURED);
    Network_Port_Changes_Activate();

    bool configuration_objects_valid = create_configuration_objects();

    create_analog_input(STATUS_AI_UPTIME_SECONDS, FW_STATUS_AI_UPTIME_NAME,
        "Seconds since this firmware booted", UNITS_SECONDS, 60.0F);
    create_analog_input(STATUS_AI_FREE_HEAP_BYTES, FW_STATUS_AI_HEAP_NAME,
        "Currently available heap memory in bytes", UNITS_NO_UNITS, 1024.0F);
    create_analog_input(STATUS_AI_MIN_HEAP_BYTES, FW_STATUS_AI_MIN_HEAP_NAME,
        "Minimum free heap observed since boot in bytes", UNITS_NO_UNITS, 1024.0F);
    create_analog_input(STATUS_AI_REBOOT_COUNT, FW_STATUS_AI_REBOOTS_NAME,
        "Persistent device boot count", UNITS_NO_UNITS, 1.0F);
    Device_Set_Database_Revision(s_config.database_revision);
    return Binary_Input_Count() == FW_DI_COUNT + FW_STATUS_BI_COUNT &&
        Binary_Output_Count() == FW_RELAY_COUNT &&
        Analog_Input_Count() == FW_STATUS_AI_COUNT &&
        Binary_Value_Count() == 1U &&
        CharacterString_Value_Count() == 1U &&
        Network_Port_Count() == 1U && configuration_objects_valid;
}

static void handler_who_is_compatible(
    uint8_t *service_request, uint16_t service_len, BACNET_ADDRESS *source)
{
    if (!s_startup_complete ||
        (s_instance.state != INSTANCE_READY && s_instance.state != INSTANCE_CLAIMING)) {
        return;
    }
    if (bip_esp32_last_receive_was_broadcast()) {
        handler_who_is(service_request, service_len, source);
    } else {
        handler_who_is_unicast(service_request, service_len, source);
    }
}

static uint64_t instance_address_key(const BACNET_ADDRESS *address)
{
    uint64_t key = 14695981039346656037ULL;
    for (unsigned i = 0; i < address->mac_len; i++) {
        key = (key ^ address->mac[i]) * 1099511628211ULL;
    }
    key = (key ^ address->net) * 1099511628211ULL;
    for (unsigned i = 0; i < address->len; i++) {
        key = (key ^ address->adr[i]) * 1099511628211ULL;
    }
    return key;
}

static void handler_instance_i_am(uint8_t *request, uint16_t length,
    BACNET_ADDRESS *source)
{
    uint32_t instance;
    unsigned max_apdu;
    int segmentation;
    uint16_t vendor;
    if (bacnet_iam_request_decode(request, length, &instance,
            &max_apdu, &segmentation, &vendor) != length) {
        return;
    }
    /* Only fills explicitly requested bindings; unsolicited inventory cannot
       consume the small address cache used for Device-ID recipients. */
    address_add_binding(instance, max_apdu, source);
    bacnet_instance_observe(&s_instance, instance, instance_address_key(source),
        (uint64_t)esp_timer_get_time() / 1000U);
}

static bool is_discovery_packet(const uint8_t *packet, uint16_t length)
{
    BACNET_ADDRESS destination = {0}, source = {0};
    BACNET_NPDU_DATA npdu = {0};
    int offset = bacnet_npdu_decode(packet, length, &destination, &source, &npdu);
    return offset > 0 && offset + 2 <= length &&
        npdu.protocol_version == BACNET_PROTOCOL_VERSION && !npdu.network_layer_message &&
        packet[offset] == PDU_TYPE_UNCONFIRMED_SERVICE_REQUEST &&
        (packet[offset + 1] == SERVICE_UNCONFIRMED_I_AM ||
         packet[offset + 1] == SERVICE_UNCONFIRMED_WHO_IS);
}

static void command_trace_begin(const BACNET_ADDRESS *peer,
    const uint8_t *packet, uint16_t length, bool processing_enabled)
{
    BACNET_ADDRESS destination = {0}, source = {0};
    BACNET_NPDU_DATA npdu = {0};
    bacnet_command_trace_end();
    int offset = bacnet_npdu_decode(packet, length, &destination, &source, &npdu);
    if (offset > 0 && offset < length &&
        npdu.protocol_version == BACNET_PROTOCOL_VERSION && !npdu.network_layer_message) {
        bacnet_command_trace_begin(peer->mac, peer->mac_len, packet + offset,
            length - offset, (uint64_t)esp_timer_get_time() / 1000U,
            processing_enabled);
    }
}

static void register_service_handlers(void)
{
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_instance_i_am);
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS,
        handler_who_is_compatible);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_READ_PROP_MULTIPLE,
        handler_read_property_multiple);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
}

static uint8_t netmask_prefix(uint32_t network_mask)
{
    uint32_t mask = ntohl(network_mask);
    uint8_t prefix = 0;
    while (mask & 0x80000000U) {
        ++prefix;
        mask <<= 1;
    }
    return mask == 0 ? prefix : 0;
}

static void update_network_port(const esp_netif_ip_info_t *info)
{
    const uint8_t *ip = (const uint8_t *)&info->ip.addr;
    const uint8_t *gateway = (const uint8_t *)&info->gw.addr;
    uint8_t dns[4] = {0};
    esp_netif_dns_info_t dns_info;
    if (ethernet_manager_get_dns_info(&dns_info)) {
        memcpy(dns, &dns_info.ip.u_addr.ip4.addr, sizeof(dns));
    }
    uint8_t bip_mac[6] = {ip[0], ip[1], ip[2], ip[3],
        (uint8_t)(s_config.bacnet_port >> 8), (uint8_t)s_config.bacnet_port};
    (void)Network_Port_IP_Address_Set(1, ip[0], ip[1], ip[2], ip[3]);
    (void)Network_Port_IP_Gateway_Set(1, gateway[0], gateway[1], gateway[2], gateway[3]);
    (void)Network_Port_IP_Subnet_Prefix_Set(1, netmask_prefix(info->netmask.addr));
    (void)Network_Port_IP_DNS_Server_Set(1, 0, dns[0], dns[1], dns[2], dns[3]);
    (void)Network_Port_MAC_Address_Set(1, bip_mac, sizeof(bip_mac));
    (void)Network_Port_Reliability_Set(1, RELIABILITY_NO_FAULT_DETECTED);
    Network_Port_Changes_Activate();
}

static void update_binary_objects(void)
{
    for (uint32_t i = 0; i < FW_DI_COUNT; ++i) {
        (void)Binary_Input_Present_Value_Set(i + 1U,
            board_io_input_get(i) ? BINARY_ACTIVE : BINARY_INACTIVE);
    }
    (void)Binary_Input_Present_Value_Set(STATUS_BI_ETHERNET_LINK,
        ethernet_manager_link_up() ? BINARY_ACTIVE : BINARY_INACTIVE);
    (void)Binary_Input_Present_Value_Set(STATUS_BI_IPV4_ASSIGNED,
        ethernet_manager_has_ip() ? BINARY_ACTIVE : BINARY_INACTIVE);
    (void)Binary_Input_Present_Value_Set(STATUS_BI_RELAY_CONTROLLER,
        board_io_relay_controller_healthy() ? BINARY_ACTIVE : BINARY_INACTIVE);
    (void)Binary_Input_Present_Value_Set(STATUS_BI_RTC_PRESENT,
        board_io_rtc_present() ? BINARY_ACTIVE : BINARY_INACTIVE);

    BACNET_RELIABILITY relay_reliability = board_io_relay_controller_healthy() ?
        RELIABILITY_NO_FAULT_DETECTED : RELIABILITY_UNRELIABLE_OTHER;
    for (uint32_t i = 1; i <= FW_RELAY_COUNT; ++i) {
        (void)Binary_Output_Reliability_Set(i, relay_reliability);
    }
}

static void update_analog_status_objects(void)
{
    float uptime = (float)(esp_timer_get_time() / 1000000ULL);
    Analog_Input_Present_Value_Set(STATUS_AI_UPTIME_SECONDS, uptime);
    Analog_Input_Present_Value_Set(STATUS_AI_FREE_HEAP_BYTES, (float)esp_get_free_heap_size());
    Analog_Input_Present_Value_Set(STATUS_AI_MIN_HEAP_BYTES,
        (float)esp_get_minimum_free_heap_size());
    Analog_Input_Present_Value_Set(STATUS_AI_REBOOT_COUNT,
        (float)config_store_reboot_count());
}

static void bacnet_task(void *context)
{
    (void)context;
    address_init();
    xSemaphoreTake(s_object_mutex, portMAX_DELAY);
    bool initialized = initialize_objects();
    if (initialized) {
        bacnet_announcement_init(&s_announcement);
        register_service_handlers();
        handler_cov_init();
        bacnet_cov_recovery_init();
        update_binary_objects();
        update_analog_status_objects();
    }
    xSemaphoreGive(s_object_mutex);
    s_start_result = initialized ? ESP_OK : ESP_FAIL;
    xSemaphoreGive(s_start_signal);
    if (!initialized) {
        ESP_LOGE(TAG, "BACnet object initialization failed");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        if (!ethernet_manager_wait_for_ip(UINT32_MAX)) {
            continue;
        }
        esp_netif_ip_info_t info;
        if (!ethernet_manager_get_ip_info(&info)) {
            continue;
        }
        uint32_t revision = ethernet_manager_network_revision();
        xSemaphoreTake(s_object_mutex, portMAX_DELAY);
        update_network_port(&info);
        xSemaphoreGive(s_object_mutex);
        bip_esp32_configure(info.ip.addr, info.netmask.addr, info.gw.addr,
            s_config.bacnet_port);
        if (!bip_init("eth0")) {
            ESP_LOGE(TAG, "Could not bind BACnet/IP UDP port %u", s_config.bacnet_port);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int64_t last_timer_us = esp_timer_get_time();
        int64_t last_second_us = last_timer_us;
        BACNET_ADDRESS own_address = {0};
        bip_get_my_address(&own_address);
        uint8_t mac[6];
        ethernet_manager_mac_get(mac);
        uint32_t seed = 2166136261U;
        for (unsigned i = 0; i < sizeof(mac); i++) {
            seed = (seed ^ mac[i]) * 16777619U;
        }
        uint32_t preferred = s_config.device_instance_mode == FW_INSTANCE_AUTO_NEW ?
            BACNET_INSTANCE_NONE : s_config.device_instance;
        bacnet_instance_start(&s_instance, config_model_instance_pending(&s_config),
            preferred, seed, instance_address_key(&own_address), (uint64_t)last_timer_us / 1000U);
        xSemaphoreTake(s_object_mutex, portMAX_DELAY);
        update_binary_objects();
        update_analog_status_objects();
        xSemaphoreGive(s_object_mutex);

        while (ethernet_manager_has_ip() &&
            ethernet_manager_network_revision() == revision) {
            BACNET_ADDRESS source = {0};
            uint16_t length = bip_receive(&source, s_pdu_buffer,
                sizeof(s_pdu_buffer), 20);
            xSemaphoreTake(s_object_mutex, portMAX_DELAY);
            update_bacnet_clock((uint64_t)esp_timer_get_time() / 1000U);
            if (length) {
                command_trace_begin(&source, s_pdu_buffer, length,
                    s_instance.state == INSTANCE_READY);
                /* Collect discovery while selecting an ID, but do not accept
                   point reads/writes or subscriptions under a provisional ID. */
                if (s_instance.state == INSTANCE_READY || is_discovery_packet(s_pdu_buffer, length)) {
                    npdu_handler(&source, s_pdu_buffer, length);
                }
                bacnet_command_trace_end();
                s_packet_count++;
            }

            int64_t now_us = esp_timer_get_time();
            uint64_t now_ms = (uint64_t)now_us / 1000U;
            unsigned actions = bacnet_instance_tick(&s_instance, now_ms);
            if (actions & INSTANCE_QUERY) {
                int32_t low = s_instance.state == INSTANCE_DISCOVERING ?
                    BACNET_INSTANCE_FIRST : s_instance.candidate;
                int32_t high = s_instance.state == INSTANCE_DISCOVERING ?
                    BACNET_INSTANCE_LAST : s_instance.candidate;
                Send_WhoIs_Local(low, high);
            }
            if (actions & INSTANCE_ANNOUNCE) {
                (void)Device_Set_Object_Instance_Number(s_instance.candidate);
                /* Provisional identity announcements belong to conflict
                   discovery, not the completed-startup announcement campaign. */
                if (s_startup_complete) {
                    Send_I_Am(&Handler_Transmit_Buffer[0]);
                }
            }
            if (actions & INSTANCE_SAVE) {
                firmware_config_t saved;
                esp_err_t result = config_store_assign_instance(s_config.database_revision,
                    s_instance.candidate, &saved);
                if (result == ESP_OK) {
                    s_config = saved;
                    (void)Device_Set_Object_Instance_Number(s_config.device_instance);
                    (void)Device_Object_Name_ANSI_Init(s_config.device_name);
                    Device_Set_Database_Revision(s_config.database_revision);
                    s_active_instance = s_config.device_instance;
                } else {
                    ESP_LOGE(TAG, "Automatic instance could not be saved: %s", esp_err_to_name(result));
                }
                bacnet_instance_saved(&s_instance, result == ESP_OK, now_ms);
                if (result == ESP_ERR_INVALID_STATE) {
                    s_instance.state = INSTANCE_CONFIG_CHANGED;
                }
            }
            if (s_startup_complete && s_instance.state == INSTANCE_READY && !s_running) {
                s_running = true;
                ESP_LOGI(TAG, "BACnet/IP Device %lu locked; listening on UDP %u",
                    (unsigned long)s_config.device_instance, s_config.bacnet_port);
            }
            s_instance_status = bacnet_instance_state_name(&s_instance);
            s_instance_conflicts = s_instance.conflicts;
            uint32_t elapsed_ms = (uint32_t)((now_us - last_timer_us) / 1000LL);
            if (elapsed_ms) {
                bacnet_cov_recovery_timer_milliseconds(elapsed_ms);
                tsm_timer_milliseconds(elapsed_ms);
                last_timer_us += (int64_t)elapsed_ms * 1000LL;
            }
            if (now_us - last_second_us >= 1000000LL) {
                uint32_t seconds = (uint32_t)((now_us - last_second_us) / 1000000LL);
                last_second_us += (int64_t)seconds * 1000000LL;
                handler_cov_timer_seconds(seconds);
                update_analog_status_objects();
            }
            while (!handler_cov_fsm()) {
                taskYIELD();
            }
            update_binary_objects();
            bool startup_ready = s_startup_complete && s_running &&
                s_instance.state == INSTANCE_READY;
            /* I-Am does not wait for NTP. The restart timestamp alone gets a
               bounded clock wait; normal BACnet reads/writes/COV keep running. */
            bacnet_announcement_tick(&s_announcement, now_ms, startup_ready,
                startup_announcement_send, NULL);
            select_restart_timestamp(now_ms, startup_ready);
            bacnet_restart_tick(now_ms, startup_ready,
                Device_Object_Instance_Number(), Device_System_Status());
            xSemaphoreGive(s_object_mutex);
        }

        xSemaphoreTake(s_object_mutex, portMAX_DELAY);
        s_running = false;
        bacnet_announcement_tick(&s_announcement,
            (uint64_t)esp_timer_get_time() / 1000U, false, NULL, NULL);
        xSemaphoreGive(s_object_mutex);
        s_instance_status = "waiting-for-network";
        bip_cleanup();
        ESP_LOGW(TAG, "BACnet/IP paused until IPv4 returns");
    }
}

esp_err_t bacnet_app_start(const firmware_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_config = *config;
    s_active_instance = config->device_instance;
    s_object_mutex = xSemaphoreCreateMutex();
    if (!s_object_mutex) {
        return ESP_ERR_NO_MEM;
    }
    s_start_signal = xSemaphoreCreateBinary();
    if (!s_start_signal) {
        vSemaphoreDelete(s_object_mutex);
        s_object_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(bacnet_task, "bacnet_ip", 10240, NULL, 6, NULL) != pdPASS) {
        vSemaphoreDelete(s_start_signal);
        s_start_signal = NULL;
        vSemaphoreDelete(s_object_mutex);
        s_object_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_start_signal, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t result = s_start_result;
    vSemaphoreDelete(s_start_signal);
    s_start_signal = NULL;
    return result;
}

bool bacnet_app_running(void)
{
    return s_running;
}

void bacnet_app_startup_complete(void)
{
    if (s_object_mutex && xSemaphoreTake(s_object_mutex, portMAX_DELAY) == pdTRUE) {
        s_startup_complete = true;
        xSemaphoreGive(s_object_mutex);
    }
}

bool bacnet_app_restart_stats_get(bacnet_restart_stats_t *stats)
{
    if (!stats || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    bacnet_restart_stats_get(stats);
    xSemaphoreGive(s_object_mutex);
    return true;
}

bool bacnet_app_announcement_stats_get(bacnet_announcement_t *stats)
{
    if (!stats || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    *stats = s_announcement;
    xSemaphoreGive(s_object_mutex);
    return true;
}

bool bacnet_app_time_stats_get(bacnet_app_time_stats_t *stats)
{
    if (!stats || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    *stats = (bacnet_app_time_stats_t){
        .timestamp_frozen = s_restart_clock.selected,
        .timestamp_from_valid_clock = s_restart_clock.from_valid_clock,
        .timestamp_source = s_restart_clock_source,
        .wait_started_ms = s_restart_clock.wait_started_ms,
        .selected_ms = s_restart_clock.selected_ms,
    };
    Device_Time_Of_Restart(&stats->timestamp);
    xSemaphoreGive(s_object_mutex);
    return true;
}

uint32_t bacnet_app_packet_count(void)
{
    return s_packet_count;
}

uint32_t bacnet_app_device_instance(void)
{
    return s_active_instance;
}

const char *bacnet_app_instance_status(void)
{
    return s_instance_status;
}

uint32_t bacnet_app_instance_conflicts(void)
{
    return s_instance_conflicts;
}

bool bacnet_app_command_trace_get(bacnet_command_trace_snapshot_t *snapshot)
{
    if (!snapshot || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    bacnet_command_trace_snapshot(snapshot);
    xSemaphoreGive(s_object_mutex);
    return true;
}

bool bacnet_app_cov_recovery_get(bacnet_cov_recovery_stats_t *stats)
{
    if (!stats || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    bacnet_cov_recovery_stats(stats);
    xSemaphoreGive(s_object_mutex);
    return true;
}

esp_err_t bacnet_app_relay_command(unsigned index, bacnet_relay_command_t command,
    unsigned priority, bacnet_relay_status_t *status)
{
    if (index >= FW_RELAY_COUNT || command > BACNET_RELAY_COMMAND_RELINQUISH ||
        priority < 1U || priority > BACNET_MAX_PRIORITY || priority == 6U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_running || !s_object_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t instance = index + 1U;
    bool updated = command == BACNET_RELAY_COMMAND_RELINQUISH ?
        Binary_Output_Present_Value_Relinquish(instance, priority) :
        Binary_Output_Present_Value_Set(instance,
            command == BACNET_RELAY_COMMAND_ON ? BINARY_ACTIVE : BINARY_INACTIVE,
            priority);
    esp_err_t result = updated ? relay_effective_value_apply(instance,
        Binary_Output_Present_Value(instance)) : ESP_ERR_INVALID_ARG;
    if (status) {
        status->active = Binary_Output_Present_Value(instance) == BINARY_ACTIVE;
        status->active_priority = Binary_Output_Present_Value_Priority(instance);
    }
    xSemaphoreGive(s_object_mutex);
    return result;
}

bool bacnet_app_relay_priorities(unsigned priorities[FW_RELAY_COUNT])
{
    if (!priorities || !s_object_mutex ||
        xSemaphoreTake(s_object_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return false;
    }
    for (unsigned i = 0; i < FW_RELAY_COUNT; ++i) {
        priorities[i] = Binary_Output_Present_Value_Priority(i + 1U);
    }
    xSemaphoreGive(s_object_mutex);
    return true;
}
