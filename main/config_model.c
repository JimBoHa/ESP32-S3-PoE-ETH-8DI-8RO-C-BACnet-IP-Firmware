/* SPDX-License-Identifier: Apache-2.0 */
#include "config_model.h"

#include <ctype.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_crc.h"
#else
static uint32_t host_crc32_le(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(crc & 1U));
        }
    }
    return ~crc;
}
#define esp_crc32_le host_crc32_le
#endif

_Static_assert(offsetof(firmware_config_t, crc32) + sizeof(uint32_t) ==
    sizeof(firmware_config_t), "configuration CRC must be the final field");

static bool text_is_terminated(const char *text, size_t capacity)
{
    return text && memchr(text, '\0', capacity) != NULL;
}

static bool hostname_is_valid(const char *hostname)
{
    size_t length = strlen(hostname);
    if (length == 0 || length > 31 || hostname[0] == '-' || hostname[length - 1] == '-') {
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)hostname[i];
        if (!(isalnum(c) || c == '-')) {
            return false;
        }
    }
    return true;
}

static bool text_is_printable_ascii(const char *text, size_t capacity, bool allow_empty)
{
    if (!text_is_terminated(text, capacity) || (!allow_empty && text[0] == '\0')) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; ++cursor) {
        if (*cursor < 0x20U || *cursor > 0x7EU) {
            return false;
        }
    }
    return true;
}

static bool ipv4_text_parse(const char *text, uint8_t bytes[4])
{
    unsigned octet[4] = {0};
    char tail = '\0';
    if (!text || sscanf(text, "%u.%u.%u.%u%c", &octet[0], &octet[1],
            &octet[2], &octet[3], &tail) != 4) {
        return false;
    }
    for (unsigned i = 0; i < 4; ++i) {
        if (octet[i] > 255U) {
            return false;
        }
        if (bytes) {
            bytes[i] = (uint8_t)octet[i];
        }
    }
    return true;
}

static uint32_t ipv4_bytes_to_u32(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
        ((uint32_t)bytes[2] << 8) | bytes[3];
}

static bool static_ipv4_is_valid(const firmware_config_t *config)
{
    uint8_t ip_bytes[4];
    uint8_t mask_bytes[4];
    uint8_t gateway_bytes[4];
    uint8_t dns_bytes[4];
    if (!ipv4_text_parse(config->ip_address, ip_bytes) ||
        !ipv4_text_parse(config->netmask, mask_bytes) ||
        !ipv4_text_parse(config->gateway, gateway_bytes) ||
        !ipv4_text_parse(config->dns_server, dns_bytes)) {
        return false;
    }
    uint32_t ip = ipv4_bytes_to_u32(ip_bytes);
    uint32_t mask = ipv4_bytes_to_u32(mask_bytes);
    uint32_t gateway = ipv4_bytes_to_u32(gateway_bytes);
    uint32_t inverse_mask = ~mask;
    bool contiguous_mask = mask != 0U && (inverse_mask & (inverse_mask + 1U)) == 0U;
    bool unicast_ip = ip != 0U && ip_bytes[0] != 0U && ip_bytes[0] != 127U &&
        ip_bytes[0] < 224U;
    if (!contiguous_mask || !unicast_ip) {
        return false;
    }
    if (inverse_mask > 1U) {
        uint32_t host = ip & inverse_mask;
        if (host == 0U || host == inverse_mask) {
            return false;
        }
    }
    if (gateway != 0U && (gateway & mask) != (ip & mask)) {
        return false;
    }
    return true;
}

static bool object_names_are_unique(const firmware_config_t *config)
{
    static const char *fixed_names[] = {
        FW_STATUS_BI_ETHERNET_NAME,
        FW_STATUS_BI_IPV4_NAME,
        FW_STATUS_BI_RELAY_NAME,
        FW_STATUS_BI_RTC_NAME,
        FW_STATUS_AI_UPTIME_NAME,
        FW_STATUS_AI_HEAP_NAME,
        FW_STATUS_AI_MIN_HEAP_NAME,
        FW_STATUS_AI_REBOOTS_NAME,
        FW_NETWORK_PORT_NAME,
        FW_CONFIG_CSV_HOSTNAME_NAME,
        FW_CONFIG_BV_RELAY_RESTORE_NAME,
    };
    const char *names[1U + FW_DI_COUNT + FW_RELAY_COUNT +
        sizeof(fixed_names) / sizeof(fixed_names[0])];
    size_t count = 0;
    names[count++] = config->device_name;
    for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
        names[count++] = config->input_names[i];
    }
    for (unsigned i = 0; i < FW_RELAY_COUNT; ++i) {
        names[count++] = config->relay_names[i];
    }
    for (unsigned i = 0; i < sizeof(fixed_names) / sizeof(fixed_names[0]); ++i) {
        names[count++] = fixed_names[i];
    }
    for (size_t left = 0; left < count; ++left) {
        for (size_t right = left + 1U; right < count; ++right) {
            if (strcasecmp(names[left], names[right]) == 0) {
                return false;
            }
        }
    }
    return true;
}

static void set_reason(char *reason, size_t size, const char *message)
{
    if (reason && size) {
        snprintf(reason, size, "%s", message);
    }
}

void config_model_defaults(firmware_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->device_instance = FW_DEFAULT_DEVICE_INSTANCE;
    config->bacnet_port = FW_DEFAULT_BACNET_PORT;
    config->vendor_id = FW_DEFAULT_VENDOR_ID;
    config->input_invert_mask = 0xFFU;
    config->dhcp_enabled = true;
    config->restore_relay_state = false;
    config->database_revision = 1;
    snprintf(config->hostname, sizeof(config->hostname), "bacnet-io-599153");
    snprintf(config->device_name, sizeof(config->device_name), "BACnet IO 599153");
    snprintf(config->vendor_name, sizeof(config->vendor_name), "%s", FW_DEFAULT_VENDOR_NAME);
    snprintf(config->location, sizeof(config->location), "Uncommissioned");
    snprintf(config->ip_address, sizeof(config->ip_address), "192.168.75.153");
    snprintf(config->netmask, sizeof(config->netmask), "255.255.255.0");
    snprintf(config->gateway, sizeof(config->gateway), "192.168.75.1");
    snprintf(config->dns_server, sizeof(config->dns_server), "192.168.75.1");
    for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
        snprintf(config->input_names[i], sizeof(config->input_names[i]), "Digital Input %u", i + 1U);
    }
    for (unsigned i = 0; i < FW_RELAY_COUNT; ++i) {
        snprintf(config->relay_names[i], sizeof(config->relay_names[i]), "Relay Output %u", i + 1U);
    }
    config_model_finalize(config);
}

bool config_model_validate(const firmware_config_t *config, char *reason, size_t reason_size)
{
    if (!config) {
        set_reason(reason, reason_size, "missing configuration");
        return false;
    }
    if (config->device_instance > 4194302U) {
        set_reason(reason, reason_size, "device_instance must be 0..4194302");
        return false;
    }
    if (config->bacnet_port == 0U) {
        set_reason(reason, reason_size, "bacnet_port must be 1..65535");
        return false;
    }
    if (!text_is_terminated(config->hostname, sizeof(config->hostname)) ||
        !hostname_is_valid(config->hostname)) {
        set_reason(reason, reason_size, "hostname must use letters, digits, or internal hyphens");
        return false;
    }
    if (!text_is_printable_ascii(config->device_name, sizeof(config->device_name), false)) {
        set_reason(reason, reason_size, "device_name must be printable ASCII");
        return false;
    }
    if (!text_is_printable_ascii(config->vendor_name, sizeof(config->vendor_name), false)) {
        set_reason(reason, reason_size, "vendor_name must be printable ASCII");
        return false;
    }
    if (!text_is_printable_ascii(config->location, sizeof(config->location), true)) {
        set_reason(reason, reason_size, "location must be printable ASCII");
        return false;
    }
    for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
        if (!text_is_printable_ascii(config->input_names[i],
            sizeof(config->input_names[i]), false)) {
            set_reason(reason, reason_size, "every input name must be printable ASCII");
            return false;
        }
    }
    for (unsigned i = 0; i < FW_RELAY_COUNT; ++i) {
        if (!text_is_printable_ascii(config->relay_names[i],
            sizeof(config->relay_names[i]), false)) {
            set_reason(reason, reason_size, "every relay name must be printable ASCII");
            return false;
        }
    }
    if (!object_names_are_unique(config)) {
        set_reason(reason, reason_size, "BACnet object names must be unique");
        return false;
    }
    if (!config->dhcp_enabled) {
        if (!static_ipv4_is_valid(config)) {
            set_reason(reason, reason_size, "static IPv4 fields are invalid");
            return false;
        }
    }
    set_reason(reason, reason_size, "ok");
    return true;
}

uint32_t config_model_crc32(const firmware_config_t *config)
{
    if (!config) {
        return 0;
    }
    return esp_crc32_le(0, (const uint8_t *)config, offsetof(firmware_config_t, crc32));
}

void config_model_finalize(firmware_config_t *config)
{
    if (!config) {
        return;
    }
    config->magic = FW_CONFIG_MAGIC;
    config->schema = FW_CONFIG_SCHEMA;
    config->size = (uint16_t)sizeof(*config);
    config->crc32 = config_model_crc32(config);
}

bool config_model_is_valid_blob(const firmware_config_t *config)
{
    if (!config || config->magic != FW_CONFIG_MAGIC ||
        config->schema != FW_CONFIG_SCHEMA || config->size != sizeof(*config) ||
        config->crc32 != config_model_crc32(config)) {
        return false;
    }
    return config_model_validate(config, NULL, 0);
}
