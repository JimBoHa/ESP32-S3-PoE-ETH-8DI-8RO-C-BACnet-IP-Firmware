/* SPDX-License-Identifier: 0BSD */
#include "time_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define TIME_CONFIG_FORMAT_VERSION 1U
#define TIME_CONFIG_HEADER_SIZE 4U
#define TIME_CONFIG_BLOB_MAX (TIME_CONFIG_HEADER_SIZE + \
    TIME_CONFIG_NTP_SERVER_SIZE - 1U + TIME_CONFIG_TIMEZONE_SIZE - 1U)

static const char *NS_CONFIG = "bacnet_cfg";
static const char *KEY_TIME_CONFIG = "time_config";
static time_config_t s_config = {
    .ntp_server = TIME_CONFIG_DEFAULT_NTP_SERVER,
    .timezone = TIME_CONFIG_DEFAULT_TIMEZONE,
};
static SemaphoreHandle_t s_mutex;
static bool s_persisted;

static bool ascii_digit(char value) { return value >= '0' && value <= '9'; }
static bool ascii_alpha(char value)
{
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
}
static bool ascii_alnum(char value) { return ascii_alpha(value) || ascii_digit(value); }

static bool reject(char *reason, size_t size, const char *message)
{
    if (reason && size) snprintf(reason, size, "%s", message);
    return false;
}

static bool server_valid(const char *server, size_t length)
{
    bool numeric = true;
    for (size_t i = 0; i < length; ++i) {
        if (!ascii_digit(server[i]) && server[i] != '.') numeric = false;
    }
    if (numeric) {
        const char *cursor = server;
        for (unsigned part = 0; part < 4; ++part) {
            unsigned value = 0, digits = 0;
            bool leading_zero = *cursor == '0';
            while (ascii_digit(*cursor)) {
                if (++digits > 3) return false;
                value = value * 10U + (unsigned)(*cursor++ - '0');
            }
            if (!digits || value > 255U || (leading_zero && digits > 1U) ||
                (part == 0 && (value == 0U || value >= 224U))) return false;
            if (part == 3) return *cursor == '\0';
            if (*cursor++ != '.') return false;
        }
        return false;
    }
    size_t label = 0;
    for (size_t i = 0; i < length; ++i) {
        char value = server[i];
        if (value == '.') {
            if (!label || server[i - 1U] == '-') return false;
            label = 0;
        } else {
            if ((!ascii_alnum(value) && value != '-') ||
                (!label && value == '-') || ++label > 63U) return false;
        }
    }
    /* A trailing root dot is allowed for an absolute DNS name. */
    return server[length - 1U] == '.' || (label && server[length - 1U] != '-');
}

static bool number(const char **cursor, unsigned minimum, unsigned maximum,
    unsigned max_digits, unsigned *parsed)
{
    unsigned value = 0, digits = 0;
    while (ascii_digit(**cursor)) {
        if (++digits > max_digits) return false;
        value = value * 10U + (unsigned)(*(*cursor)++ - '0');
    }
    if (!digits || value < minimum || value > maximum) return false;
    if (parsed) *parsed = value;
    return true;
}

static bool abbreviation(const char **cursor)
{
    bool quoted = **cursor == '<';
    if (quoted) ++*cursor;
    unsigned length = 0;
    while (ascii_alpha(**cursor) ||
        (quoted && (ascii_digit(**cursor) || **cursor == '+' || **cursor == '-'))) {
        ++*cursor;
        if (++length > 10U) return false; /* ESP-IDF newlib TZNAME_MAX. */
    }
    if (length < 3U) return false;
    if (quoted) {
        if (**cursor != '>') return false;
        ++*cursor;
    }
    return true;
}

static bool hours_minutes_seconds(const char **cursor, bool signed_value, bool offset)
{
    if (signed_value && (**cursor == '+' || **cursor == '-')) ++*cursor;
    unsigned hours = 0, minutes = 0, seconds = 0;
    if (!number(cursor, 0, 24, 2, &hours)) return false;
    if (**cursor == ':') {
        ++*cursor;
        if (!number(cursor, 0, 59, 2, &minutes)) return false;
        if (**cursor == ':') {
            ++*cursor;
            if (!number(cursor, 0, 59, 2, &seconds)) return false;
        }
    }
    /* BACnet UTC_Offset has minute precision; do not silently round offsets. */
    return (!offset || seconds == 0U) &&
        (hours < 24U || (minutes == 0U && seconds == 0U));
}

static bool transition(const char **cursor)
{
    if (**cursor == 'M') {
        ++*cursor;
        if (!number(cursor, 1, 12, 2, NULL) || **cursor != '.') return false;
        ++*cursor;
        if (!number(cursor, 1, 5, 1, NULL) || **cursor != '.') return false;
        ++*cursor;
        if (!number(cursor, 0, 6, 1, NULL)) return false;
    } else if (**cursor == 'J') {
        ++*cursor;
        if (!number(cursor, 1, 365, 3, NULL)) return false;
    } else if (!number(cursor, 0, 365, 3, NULL)) {
        return false;
    }
    if (**cursor == '/') {
        ++*cursor;
        if (!hours_minutes_seconds(cursor, false, false)) return false;
    }
    return true;
}

static bool timezone_valid(const char *timezone)
{
    const char *cursor = timezone;
    if (!abbreviation(&cursor) || !hours_minutes_seconds(&cursor, true, true)) return false;
    if (!*cursor) return true;
    if (!abbreviation(&cursor)) return false;
    if (*cursor != ',' && !hours_minutes_seconds(&cursor, true, true)) return false;
    /* Explicit rules avoid silently adopting newlib's US DST defaults. */
    if (*cursor != ',') return false;
    ++cursor;
    if (!transition(&cursor) || *cursor != ',') return false;
    ++cursor;
    return transition(&cursor) && *cursor == '\0';
}

bool time_config_validate(const time_config_t *config, char *reason, size_t reason_size)
{
    if (!config) return reject(reason, reason_size, "time configuration is required");
    const char *server_end = memchr(config->ntp_server, '\0', sizeof(config->ntp_server));
    const char *zone_end = memchr(config->timezone, '\0', sizeof(config->timezone));
    if (!server_end || server_end == config->ntp_server ||
        !server_valid(config->ntp_server, (size_t)(server_end - config->ntp_server))) {
        return reject(reason, reason_size,
            "ntp_server must be a hostname or unicast IPv4 address, without URL or port (max 253 characters)");
    }
    if (!zone_end || zone_end == config->timezone || !timezone_valid(config->timezone)) {
        return reject(reason, reason_size,
            "timezone must be a POSIX TZ rule with explicit DST transitions, not an IANA path (max 95 characters)");
    }
    if (reason && reason_size) reason[0] = '\0';
    return true;
}

static bool decode(const uint8_t *blob, size_t length, time_config_t *config)
{
    if (length < TIME_CONFIG_HEADER_SIZE || blob[0] != TIME_CONFIG_FORMAT_VERSION) return false;
    size_t server_length = (size_t)blob[1] | ((size_t)blob[2] << 8U);
    size_t zone_length = blob[3];
    if (!server_length || server_length >= sizeof(config->ntp_server) ||
        !zone_length || zone_length >= sizeof(config->timezone) ||
        length != TIME_CONFIG_HEADER_SIZE + server_length + zone_length ||
        memchr(blob + TIME_CONFIG_HEADER_SIZE, '\0', server_length + zone_length)) return false;
    memset(config, 0, sizeof(*config));
    memcpy(config->ntp_server, blob + TIME_CONFIG_HEADER_SIZE, server_length);
    memcpy(config->timezone, blob + TIME_CONFIG_HEADER_SIZE + server_length, zone_length);
    return time_config_validate(config, NULL, 0);
}

esp_err_t time_config_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    time_config_t candidate = {
        .ntp_server = TIME_CONFIG_DEFAULT_NTP_SERVER,
        .timezone = TIME_CONFIG_DEFAULT_TIMEZONE,
    };
    bool loaded = false;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        uint8_t blob[TIME_CONFIG_BLOB_MAX];
        size_t length = sizeof(blob);
        result = nvs_get_blob(handle, KEY_TIME_CONFIG, blob, &length);
        nvs_close(handle);
        if (result == ESP_OK) loaded = decode(blob, length, &candidate);
    }
    if (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND || result == ESP_ERR_NVS_INVALID_LENGTH) {
        if (!loaded) {
            memset(&candidate, 0, sizeof(candidate));
            strcpy(candidate.ntp_server, TIME_CONFIG_DEFAULT_NTP_SERVER);
            strcpy(candidate.timezone, TIME_CONFIG_DEFAULT_TIMEZONE);
        }
        s_config = candidate;
        s_persisted = loaded;
        result = ESP_OK;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

void time_config_get(time_config_t *config)
{
    if (!config) return;
    /* Safe defaults remain readable if optional NVS initialization failed. */
    if (!s_mutex) { *config = s_config; return; }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        memset(config, 0, sizeof(*config));
        strcpy(config->ntp_server, TIME_CONFIG_DEFAULT_NTP_SERVER);
        strcpy(config->timezone, TIME_CONFIG_DEFAULT_TIMEZONE);
        return;
    }
    *config = s_config;
    xSemaphoreGive(s_mutex);
}

esp_err_t time_config_update(const time_config_t *config)
{
    if (!s_mutex || !time_config_validate(config, NULL, 0)) return ESP_ERR_INVALID_ARG;
    time_config_t candidate = {0};
    strcpy(candidate.ntp_server, config->ntp_server);
    strcpy(candidate.timezone, config->timezone);
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_persisted && !memcmp(&candidate, &s_config, sizeof(candidate))) {
        xSemaphoreGive(s_mutex);
        return ESP_OK;
    }
    size_t server_length = strlen(candidate.ntp_server), zone_length = strlen(candidate.timezone);
    uint8_t blob[TIME_CONFIG_BLOB_MAX] = {
        TIME_CONFIG_FORMAT_VERSION, (uint8_t)server_length,
        (uint8_t)(server_length >> 8U), (uint8_t)zone_length,
    };
    memcpy(blob + TIME_CONFIG_HEADER_SIZE, candidate.ntp_server, server_length);
    memcpy(blob + TIME_CONFIG_HEADER_SIZE + server_length, candidate.timezone, zone_length);
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_set_blob(handle, KEY_TIME_CONFIG, blob,
            TIME_CONFIG_HEADER_SIZE + server_length + zone_length);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
    }
    if (result == ESP_OK) {
        s_config = candidate;
        s_persisted = true;
    }
    xSemaphoreGive(s_mutex);
    return result;
}
