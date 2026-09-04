/* SPDX-License-Identifier: Apache-2.0 */
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

#include "bacnet/apdu.h"
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
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/cov.h"
#include "bacnet/datalink/bip.h"
#include "bacnet/npdu.h"

#include "bip_esp32.h"
#include "board_io.h"
#include "config_store.h"
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
static volatile uint32_t s_packet_count;
static uint8_t s_pdu_buffer[BIP_MPDU_MAX];
static SemaphoreHandle_t s_start_signal;
static SemaphoreHandle_t s_object_mutex;
static esp_err_t s_start_result;

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

static object_functions_t s_object_table[] = {
    {OBJECT_DEVICE, NULL, Device_Count, Device_Index_To_Instance,
        Device_Valid_Object_Instance_Number, Device_Object_Name,
        Device_Read_Property_Local, NULL,
        Device_Property_Lists, DeviceGetRRInfo, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL, NULL, NULL},
    {OBJECT_BINARY_INPUT, Binary_Input_Init, Binary_Input_Count,
        Binary_Input_Index_To_Instance, Binary_Input_Valid_Instance,
        Binary_Input_Object_Name, Binary_Input_Read_Property, NULL,
        Binary_Input_Property_Lists, NULL, NULL, binary_input_encode_value_list,
        Binary_Input_Change_Of_Value, Binary_Input_Change_Of_Value_Clear,
        NULL, NULL, NULL, Binary_Input_Create, Binary_Input_Delete, NULL,
        read_only_writable_property_list},
    {OBJECT_ANALOG_INPUT, Analog_Input_Init, Analog_Input_Count,
        Analog_Input_Index_To_Instance, Analog_Input_Valid_Instance,
        Analog_Input_Object_Name, Analog_Input_Read_Property,
        NULL, Analog_Input_Property_Lists, NULL, NULL,
        Analog_Input_Encode_Value_List, Analog_Input_Change_Of_Value,
        Analog_Input_Change_Of_Value_Clear, Analog_Input_Intrinsic_Reporting,
        NULL, NULL, Analog_Input_Create, Analog_Input_Delete, NULL,
        NULL},
    {OBJECT_BINARY_OUTPUT, Binary_Output_Init, Binary_Output_Count,
        Binary_Output_Index_To_Instance, Binary_Output_Valid_Instance,
        Binary_Output_Object_Name, Binary_Output_Read_Property,
        binary_output_write_property, Binary_Output_Property_Lists, NULL, NULL,
        Binary_Output_Encode_Value_List, Binary_Output_Change_Of_Value,
        Binary_Output_Change_Of_Value_Clear, NULL, NULL, NULL,
        Binary_Output_Create, Binary_Output_Delete, NULL,
        binary_output_writable_property_list},
    {OBJECT_BINARY_VALUE, Binary_Value_Init, Binary_Value_Count,
        Binary_Value_Index_To_Instance, Binary_Value_Valid_Instance,
        Binary_Value_Object_Name, Binary_Value_Read_Property,
        read_only_write_property, Binary_Value_Property_Lists, NULL, NULL,
        Binary_Value_Encode_Value_List, Binary_Value_Change_Of_Value,
        Binary_Value_Change_Of_Value_Clear, NULL, NULL, NULL, NULL, NULL, NULL,
        read_only_writable_property_list},
    {OBJECT_CHARACTERSTRING_VALUE, CharacterString_Value_Init,
        CharacterString_Value_Count, CharacterString_Value_Index_To_Instance,
        CharacterString_Value_Valid_Instance, CharacterString_Value_Object_Name,
        CharacterString_Value_Read_Property, read_only_write_property,
        CharacterString_Value_Property_Lists, NULL, NULL,
        CharacterString_Value_Encode_Value_List,
        CharacterString_Value_Change_Of_Value,
        CharacterString_Value_Change_Of_Value_Clear, NULL, NULL, NULL, NULL,
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

    uint8_t mac[6];
    char serial[18];
    ethernet_manager_mac_get(mac);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    (void)Device_Serial_Number_Set(serial, strlen(serial));

    for (uint32_t i = 0; i < FW_DI_COUNT; ++i) {
        char description_text[64];
        bool inverted = (s_config.input_invert_mask & (1U << i)) != 0;
        snprintf(description_text, sizeof(description_text),
            "Opto-isolated digital input DI%lu; configured active-%s",
            (unsigned long)(i + 1U), inverted ? "low" : "high");
        create_binary_input(i + 1U, s_config.input_names[i], description_text);
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
        char output_description[80];
        snprintf(output_description, sizeof(output_description),
            "Relay RO%lu command; state is commanded, not contact feedback",
            (unsigned long)instance);
        (void)Binary_Output_Description_Set(instance, output_description);
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
    if (bip_esp32_last_receive_was_broadcast()) {
        handler_who_is(service_request, service_len, source);
    } else {
        handler_who_is_unicast(service_request, service_len, source);
    }
}

static void register_service_handlers(void)
{
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
        register_service_handlers();
        handler_cov_init();
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
        xSemaphoreTake(s_object_mutex, portMAX_DELAY);
        update_binary_objects();
        update_analog_status_objects();
        xSemaphoreGive(s_object_mutex);
        s_running = true;
        ESP_LOGI(TAG, "BACnet/IP Device %lu listening on UDP %u",
            (unsigned long)s_config.device_instance, s_config.bacnet_port);
        xSemaphoreTake(s_object_mutex, portMAX_DELAY);
        Send_I_Am(&Handler_Transmit_Buffer[0]);
        xSemaphoreGive(s_object_mutex);

        while (ethernet_manager_has_ip() &&
            ethernet_manager_network_revision() == revision) {
            BACNET_ADDRESS source = {0};
            uint16_t length = bip_receive(&source, s_pdu_buffer,
                sizeof(s_pdu_buffer), 20);
            xSemaphoreTake(s_object_mutex, portMAX_DELAY);
            if (length) {
                npdu_handler(&source, s_pdu_buffer, length);
                s_packet_count++;
            }

            int64_t now_us = esp_timer_get_time();
            uint32_t elapsed_ms = (uint32_t)((now_us - last_timer_us) / 1000LL);
            if (elapsed_ms) {
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
            xSemaphoreGive(s_object_mutex);
        }

        s_running = false;
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

uint32_t bacnet_app_packet_count(void)
{
    return s_packet_count;
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
