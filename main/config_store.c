/* SPDX-License-Identifier: 0BSD */
#include "config_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "config_store";
static const char *NS_CONFIG = "bacnet_cfg";
static const char *NS_SECURITY = "bacnet_sec";
static const char *NS_RUNTIME = "bacnet_run";
static const char *KEY_RESTART_RECIPIENTS = "restart_rcpt";

#define RESTART_RECIPIENTS_FORMAT_VERSION 1U
#define RESTART_RECIPIENTS_HEADER_SIZE 3U

static firmware_config_t s_config;
static SemaphoreHandle_t s_mutex;
static uint32_t s_reboot_count;

static esp_err_t write_config_blob(const firmware_config_t *config)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_blob(handle, "config", config, sizeof(*config));
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t update_reboot_count(void)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_RUNTIME, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    uint32_t count = 0;
    result = nvs_get_u32(handle, "boots", &count);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    }
    if (result == ESP_OK) {
        count++;
        result = nvs_set_u32(handle, "boots", count);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    if (result == ESP_OK) {
        s_reboot_count = count;
    }
    return result;
}

esp_err_t config_store_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    bool loaded = false;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        size_t length = sizeof(s_config);
        result = nvs_get_blob(handle, "config", &s_config, &length);
        nvs_close(handle);
        loaded = result == ESP_OK && length == sizeof(s_config) &&
            config_model_is_valid_blob(&s_config);
    }

    if (loaded && config_model_migrate_instance_mode(&s_config)) {
        result = write_config_blob(&s_config);
        if (result != ESP_OK) {
            return result;
        }
    }
    if (!loaded) {
        config_model_defaults(&s_config);
        result = write_config_blob(&s_config);
        if (result != ESP_OK) {
            return result;
        }
        ESP_LOGW(TAG, "No valid saved configuration; installed safe defaults");
    }

    result = update_reboot_count();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist reboot count: %s", esp_err_to_name(result));
        s_reboot_count = 1;
    }
    return ESP_OK;
}

void config_store_get(firmware_config_t *config)
{
    if (!config || !s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_mutex);
}

esp_err_t config_store_update(const firmware_config_t *config)
{
    if (!config || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    firmware_config_t candidate = *config;
    char reason[96];
    if (!config_model_validate(&candidate, reason, sizeof(reason))) {
        ESP_LOGW(TAG, "Rejected invalid configuration: %s", reason);
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (candidate.database_revision != s_config.database_revision) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    candidate.database_revision = s_config.database_revision + 1U;
    config_model_finalize(&candidate);
    esp_err_t result = write_config_blob(&candidate);
    if (result == ESP_OK) {
        s_config = candidate;
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t config_store_assign_instance(uint32_t expected_revision,
    uint32_t instance, firmware_config_t *saved)
{
    if (!s_mutex || !saved) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    firmware_config_t candidate = s_config;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (config_model_assign_instance(&candidate, expected_revision, instance)) {
        result = write_config_blob(&candidate);
        if (result == ESP_OK) {
            s_config = candidate;
            *saved = candidate;
        }
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t config_store_admin_key_get(uint8_t key[FW_AUTH_KEY_BYTES], bool *created)
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    if (created) {
        *created = false;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_SECURITY, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    size_t length = FW_AUTH_KEY_BYTES;
    result = nvs_get_blob(handle, "admin_key", key, &length);
    if (result == ESP_ERR_NVS_NOT_FOUND || length != FW_AUTH_KEY_BYTES) {
        esp_fill_random(key, FW_AUTH_KEY_BYTES);
        result = nvs_set_blob(handle, "admin_key", key, FW_AUTH_KEY_BYTES);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        if (result == ESP_OK && created) {
            *created = true;
        }
    }
    nvs_close(handle);
    return result;
}

uint32_t config_store_reboot_count(void)
{
    return s_reboot_count;
}

uint8_t config_store_relay_state_get(void)
{
    nvs_handle_t handle;
    uint8_t state = 0;
    if (nvs_open(NS_RUNTIME, NVS_READONLY, &handle) == ESP_OK) {
        (void)nvs_get_u8(handle, "relays", &state);
        nvs_close(handle);
    }
    return state;
}

esp_err_t config_store_relay_state_set(uint8_t relay_state)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_RUNTIME, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_u8(handle, "relays", relay_state);
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

/* NVS provides blob integrity; this explicit version/length header avoids
   persisting compiler layouts, padding, pointers, or the base config schema. */
static esp_err_t restart_recipients_read(
    nvs_handle_t handle, uint8_t *encoded, size_t capacity, size_t *length)
{
    uint8_t blob[RESTART_RECIPIENTS_HEADER_SIZE + CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES];
    size_t blob_length = sizeof(blob);
    esp_err_t result = nvs_get_blob(handle, KEY_RESTART_RECIPIENTS, blob, &blob_length);
    if (result != ESP_OK) {
        return result;
    }
    if (blob_length < RESTART_RECIPIENTS_HEADER_SIZE ||
        blob[0] != RESTART_RECIPIENTS_FORMAT_VERSION) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t payload_length = (size_t)blob[1] | ((size_t)blob[2] << 8);
    if (payload_length > CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES ||
        blob_length != RESTART_RECIPIENTS_HEADER_SIZE + payload_length) {
        return ESP_ERR_INVALID_STATE;
    }
    if (payload_length > capacity) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (payload_length) {
        memcpy(encoded, blob + RESTART_RECIPIENTS_HEADER_SIZE, payload_length);
    }
    *length = payload_length;
    return ESP_OK;
}

esp_err_t config_store_restart_recipients_get(
    uint8_t *encoded, size_t capacity, size_t *length)
{
    if (length) {
        *length = 0;
    }
    if (!length || (!encoded && capacity) || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        result = restart_recipients_read(handle, encoded, capacity, length);
        nvs_close(handle);
    }
    xSemaphoreGive(s_mutex);
    return result;
}

esp_err_t config_store_restart_recipients_set(const uint8_t *encoded, size_t length)
{
    if ((!encoded && length) || !s_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    if (length > CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NS_CONFIG, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        uint8_t previous[CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES];
        size_t previous_length = 0;
        result = restart_recipients_read(handle, previous, sizeof(previous), &previous_length);
        bool identical = result == ESP_OK && previous_length == length &&
            (length == 0 || memcmp(previous, encoded, length) == 0);
        if (!identical && (result == ESP_OK || result == ESP_ERR_NVS_NOT_FOUND ||
                result == ESP_ERR_NVS_INVALID_LENGTH || result == ESP_ERR_INVALID_STATE)) {
            uint8_t blob[RESTART_RECIPIENTS_HEADER_SIZE + CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES];
            blob[0] = RESTART_RECIPIENTS_FORMAT_VERSION;
            blob[1] = (uint8_t)length;
            blob[2] = (uint8_t)(length >> 8);
            if (length) {
                memcpy(blob + RESTART_RECIPIENTS_HEADER_SIZE, encoded, length);
            }
            result = nvs_set_blob(handle, KEY_RESTART_RECIPIENTS, blob,
                RESTART_RECIPIENTS_HEADER_SIZE + length);
            if (result == ESP_OK) {
                result = nvs_commit(handle);
            }
        }
        nvs_close(handle);
    }
    xSemaphoreGive(s_mutex);
    return result;
}
