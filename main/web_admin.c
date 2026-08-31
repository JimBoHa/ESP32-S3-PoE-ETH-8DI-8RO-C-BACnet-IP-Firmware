/* SPDX-License-Identifier: Apache-2.0 */
#include "web_admin.h"

#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"

#include "auth.h"
#include "bacnet_app.h"
#include "board_io.h"
#include "config_store.h"
#include "ethernet_manager.h"
#include "firmware.h"

#define CONFIG_BODY_MAX 8192U
#define OTA_RECEIVE_BUFFER 4096U
#define CONFIG_RECEIVE_DEADLINE_US (15LL * 1000LL * 1000LL)
#define OTA_RECEIVE_DEADLINE_US (5LL * 60LL * 1000LL * 1000LL)

static const char *TAG = "web_admin";
static httpd_handle_t s_server;

static esp_err_t send_json_text(httpd_req_t *request, const char *status, const char *json)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, json);
}

static esp_err_t send_json_object(httpd_req_t *request, const char *status, cJSON *object)
{
    char *json = cJSON_PrintUnformatted(object);
    if (!json) {
        return send_json_text(request, "500 Internal Server Error", "{\"error\":\"out of memory\"}");
    }
    esp_err_t result = send_json_text(request, status, json);
    free(json);
    return result;
}

static esp_err_t send_error(httpd_req_t *request, const char *status, const char *message)
{
    cJSON *object = cJSON_CreateObject();
    if (!object) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(object, "error", message);
    esp_err_t result = send_json_object(request, status, object);
    cJSON_Delete(object);
    return result;
}

static bool request_header(httpd_req_t *request, const char *name, char *value, size_t size)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    return length > 0 && length < size &&
        httpd_req_get_hdr_value_str(request, name, value, size) == ESP_OK;
}

static bool request_authorize(httpd_req_t *request, const char *method, const char *path,
    size_t content_length, const char *verified_body_hash, char *reason, size_t reason_size)
{
    char nonce[FW_AUTH_HEX_NONCE_LEN + 1];
    char claimed_hash[FW_AUTH_HEX_SHA256_LEN + 1];
    char signature[FW_AUTH_HEX_SHA256_LEN + 1];
    if (!request_header(request, "X-Auth-Nonce", nonce, sizeof(nonce)) ||
        !request_header(request, "X-Content-SHA256", claimed_hash, sizeof(claimed_hash)) ||
        !request_header(request, "X-Authorization", signature, sizeof(signature))) {
        snprintf(reason, reason_size, "missing authentication headers");
        return false;
    }
    if (verified_body_hash && strcasecmp(claimed_hash, verified_body_hash) != 0) {
        snprintf(reason, reason_size, "content SHA-256 mismatch");
        return false;
    }
    return auth_request_verify(method, path, nonce, content_length, claimed_hash,
        signature, reason, reason_size);
}

static esp_err_t root_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><meta charset=utf-8><title>BACnet I/O</title>"
        "<h1>ESP32-S3 BACnet/IP I/O</h1>"
        "<p>Read-only status: <a href=/api/v1/status>/api/v1/status</a></p>"
        "<p>Configuration and OTA mutations require HMAC authentication. "
        "Use <code>tools/device_admin.py</code>.</p>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *request)
{
    firmware_config_t config;
    config_store_get(&config);
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_netif_ip_info_t info = {0};
    bool has_ip = ethernet_manager_get_ip_info(&info);
    char ip[16] = "0.0.0.0";
    if (has_ip) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "product", FW_PRODUCT_NAME);
    cJSON_AddStringToObject(root, "firmware_version", app->version);
    cJSON_AddStringToObject(root, "build_date", app->date);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "unknown");
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(root, "reboot_count", config_store_reboot_count());
    cJSON_AddStringToObject(root, "ip_address", ip);
    cJSON_AddBoolToObject(root, "ethernet_link", ethernet_manager_link_up());
    cJSON_AddBoolToObject(root, "ipv4_assigned", has_ip);
    cJSON_AddBoolToObject(root, "bacnet_running", bacnet_app_running());
    cJSON_AddNumberToObject(root, "bacnet_device_instance", config.device_instance);
    cJSON_AddNumberToObject(root, "bacnet_udp_port", config.bacnet_port);
    cJSON_AddNumberToObject(root, "bacnet_vendor_id", config.vendor_id);
    cJSON_AddNumberToObject(root, "bacnet_packets_received", bacnet_app_packet_count());
    cJSON_AddNumberToObject(root, "digital_inputs_mask", board_io_inputs_mask());
    cJSON_AddNumberToObject(root, "relay_outputs_mask", board_io_relays_mask());
    cJSON_AddNumberToObject(root, "relay_commands_mask", board_io_relay_commands_mask());
    cJSON_AddBoolToObject(root, "relay_controller_healthy", board_io_relay_controller_healthy());
    cJSON_AddBoolToObject(root, "rtc_present", board_io_rtc_present());
    cJSON_AddNumberToObject(root, "free_heap_bytes", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "minimum_free_heap_bytes", esp_get_minimum_free_heap_size());

    esp_err_t result = send_json_object(request, "200 OK", root);
    cJSON_Delete(root);
    return result;
}

static esp_err_t challenge_handler(httpd_req_t *request)
{
    char nonce[FW_AUTH_HEX_NONCE_LEN + 1];
    ESP_RETURN_ON_ERROR(auth_challenge_issue(nonce), TAG, "issue auth challenge");
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "scheme", "HMAC-SHA256");
    cJSON_AddStringToObject(root, "canonical_version", "BACNET-IO-AUTH-V1");
    cJSON_AddStringToObject(root, "nonce", nonce);
    cJSON_AddNumberToObject(root, "expires_in_seconds", FW_AUTH_CHALLENGE_TTL_SECONDS);
    esp_err_t result = send_json_object(request, "200 OK", root);
    cJSON_Delete(root);
    return result;
}

static cJSON *config_to_json(const firmware_config_t *config)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddNumberToObject(root, "schema", config->schema);
    cJSON_AddNumberToObject(root, "database_revision", config->database_revision);
    cJSON_AddNumberToObject(root, "device_instance", config->device_instance);
    cJSON_AddNumberToObject(root, "bacnet_port", config->bacnet_port);
    cJSON_AddNumberToObject(root, "vendor_id", config->vendor_id);
    cJSON_AddNumberToObject(root, "input_invert_mask", config->input_invert_mask);
    cJSON_AddBoolToObject(root, "dhcp_enabled", config->dhcp_enabled);
    cJSON_AddBoolToObject(root, "restore_relay_state", config->restore_relay_state);
    cJSON_AddStringToObject(root, "hostname", config->hostname);
    cJSON_AddStringToObject(root, "device_name", config->device_name);
    cJSON_AddStringToObject(root, "vendor_name", config->vendor_name);
    cJSON_AddStringToObject(root, "location", config->location);
    cJSON_AddStringToObject(root, "ip_address", config->ip_address);
    cJSON_AddStringToObject(root, "netmask", config->netmask);
    cJSON_AddStringToObject(root, "gateway", config->gateway);
    cJSON_AddStringToObject(root, "dns_server", config->dns_server);
    cJSON *inputs = cJSON_AddArrayToObject(root, "input_names");
    cJSON *relays = cJSON_AddArrayToObject(root, "relay_names");
    for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
        cJSON_AddItemToArray(inputs, cJSON_CreateString(config->input_names[i]));
    }
    for (unsigned i = 0; i < FW_RELAY_COUNT; ++i) {
        cJSON_AddItemToArray(relays, cJSON_CreateString(config->relay_names[i]));
    }
    return root;
}

static esp_err_t config_get_handler(httpd_req_t *request)
{
    firmware_config_t config;
    config_store_get(&config);
    cJSON *json = config_to_json(&config);
    if (!json) {
        return send_error(request, "500 Internal Server Error", "out of memory");
    }
    esp_err_t result = send_json_object(request, "200 OK", json);
    cJSON_Delete(json);
    return result;
}

static bool json_copy_string(cJSON *root, const char *key, char *destination,
    size_t capacity, char *reason, size_t reason_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value) {
        return true;
    }
    if (!cJSON_IsString(value) || !value->valuestring ||
        strlen(value->valuestring) >= capacity) {
        snprintf(reason, reason_size, "%s must be a shorter string", key);
        return false;
    }
    snprintf(destination, capacity, "%s", value->valuestring);
    return true;
}

static bool json_copy_number(cJSON *root, const char *key, uint32_t max,
    uint32_t *destination, char *reason, size_t reason_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value) {
        return true;
    }
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 ||
        value->valuedouble > max || value->valuedouble != (double)(uint32_t)value->valuedouble) {
        snprintf(reason, reason_size, "%s is out of range", key);
        return false;
    }
    *destination = (uint32_t)value->valuedouble;
    return true;
}

static bool json_copy_bool(cJSON *root, const char *key, bool *destination,
    char *reason, size_t reason_size)
{
    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!value) {
        return true;
    }
    if (!cJSON_IsBool(value)) {
        snprintf(reason, reason_size, "%s must be boolean", key);
        return false;
    }
    *destination = cJSON_IsTrue(value);
    return true;
}

static bool json_copy_name_array(cJSON *root, const char *key,
    char names[][FW_NAME_LEN], unsigned count, char *reason, size_t reason_size)
{
    cJSON *array = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!array) {
        return true;
    }
    if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != (int)count) {
        snprintf(reason, reason_size, "%s must contain exactly %u strings", key, count);
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        cJSON *item = cJSON_GetArrayItem(array, (int)i);
        if (!cJSON_IsString(item) || !item->valuestring || item->valuestring[0] == '\0' ||
            strlen(item->valuestring) >= FW_NAME_LEN) {
            snprintf(reason, reason_size, "%s[%u] is invalid", key, i);
            return false;
        }
        snprintf(names[i], FW_NAME_LEN, "%s", item->valuestring);
    }
    return true;
}

static uint8_t *receive_body(httpd_req_t *request, size_t maximum, size_t *length)
{
    if (request->content_len <= 0 || (size_t)request->content_len > maximum) {
        return NULL;
    }
    size_t expected = (size_t)request->content_len;
    uint8_t *body = malloc(expected + 1U);
    if (!body) {
        return NULL;
    }
    size_t received = 0;
    int64_t deadline_us = esp_timer_get_time() + CONFIG_RECEIVE_DEADLINE_US;
    while (received < expected) {
        int result = httpd_req_recv(request, (char *)&body[received], expected - received);
        if (result == HTTPD_SOCK_ERR_TIMEOUT) {
            if (esp_timer_get_time() >= deadline_us) {
                free(body);
                return NULL;
            }
            continue;
        }
        if (result <= 0) {
            free(body);
            return NULL;
        }
        received += (size_t)result;
    }
    body[received] = 0;
    *length = received;
    return body;
}

static esp_err_t config_put_handler(httpd_req_t *request)
{
    size_t body_length = 0;
    uint8_t *body = receive_body(request, CONFIG_BODY_MAX, &body_length);
    if (!body) {
        return send_error(request, "400 Bad Request", "invalid configuration body");
    }
    char body_hash[FW_AUTH_HEX_SHA256_LEN + 1];
    char reason[128];
    if (!auth_sha256_hex(body, body_length, body_hash) ||
        !request_authorize(request, "PUT", "/api/v1/config", body_length,
            body_hash, reason, sizeof(reason))) {
        free(body);
        return send_error(request, "401 Unauthorized", reason);
    }

    cJSON *json = cJSON_ParseWithLength((char *)body, body_length);
    free(body);
    if (!json || !cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return send_error(request, "400 Bad Request", "body must be a JSON object");
    }

    firmware_config_t config;
    config_store_get(&config);
    uint32_t number = 0;
    bool valid =
        json_copy_number(json, "device_instance", 4194302U, &config.device_instance, reason, sizeof(reason)) &&
        json_copy_number(json, "bacnet_port", UINT16_MAX, &number, reason, sizeof(reason));
    if (valid && cJSON_GetObjectItemCaseSensitive(json, "bacnet_port")) {
        config.bacnet_port = (uint16_t)number;
    }
    number = config.vendor_id;
    valid = valid && json_copy_number(json, "vendor_id", UINT16_MAX,
        &number, reason, sizeof(reason));
    config.vendor_id = (uint16_t)number;
    number = config.input_invert_mask;
    valid = valid && json_copy_number(json, "input_invert_mask", UINT8_MAX, &number, reason, sizeof(reason));
    config.input_invert_mask = (uint8_t)number;
    valid = valid &&
        json_copy_bool(json, "dhcp_enabled", &config.dhcp_enabled, reason, sizeof(reason)) &&
        json_copy_bool(json, "restore_relay_state", &config.restore_relay_state, reason, sizeof(reason)) &&
        json_copy_string(json, "hostname", config.hostname, sizeof(config.hostname), reason, sizeof(reason)) &&
        json_copy_string(json, "device_name", config.device_name, sizeof(config.device_name), reason, sizeof(reason)) &&
        json_copy_string(json, "vendor_name", config.vendor_name, sizeof(config.vendor_name), reason, sizeof(reason)) &&
        json_copy_string(json, "location", config.location, sizeof(config.location), reason, sizeof(reason)) &&
        json_copy_string(json, "ip_address", config.ip_address, sizeof(config.ip_address), reason, sizeof(reason)) &&
        json_copy_string(json, "netmask", config.netmask, sizeof(config.netmask), reason, sizeof(reason)) &&
        json_copy_string(json, "gateway", config.gateway, sizeof(config.gateway), reason, sizeof(reason)) &&
        json_copy_string(json, "dns_server", config.dns_server, sizeof(config.dns_server), reason, sizeof(reason)) &&
        json_copy_name_array(json, "input_names", config.input_names, FW_DI_COUNT, reason, sizeof(reason)) &&
        json_copy_name_array(json, "relay_names", config.relay_names, FW_RELAY_COUNT, reason, sizeof(reason)) &&
        config_model_validate(&config, reason, sizeof(reason));
    cJSON_Delete(json);
    if (!valid) {
        return send_error(request, "400 Bad Request", reason);
    }

    esp_err_t result = config_store_update(&config);
    if (result != ESP_OK) {
        return send_error(request, "500 Internal Server Error", esp_err_to_name(result));
    }
    return send_json_text(request, "200 OK",
        "{\"saved\":true,\"persistent\":true,\"reboot_required\":true}");
}

static void delayed_restart_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *request)
{
    static const uint8_t empty_body = 0;
    char empty_hash[FW_AUTH_HEX_SHA256_LEN + 1];
    char reason[128];
    if (!auth_sha256_hex(&empty_body, 0, empty_hash)) {
        return send_error(request, "500 Internal Server Error", "SHA-256 failed");
    }
    if (!request_authorize(request, "POST", "/api/v1/reboot", 0,
        empty_hash, reason, sizeof(reason))) {
        return send_error(request, "401 Unauthorized", reason);
    }
    if (xTaskCreate(delayed_restart_task, "delayed_restart", 2048,
        NULL, 4, NULL) != pdPASS) {
        return send_error(request, "500 Internal Server Error", "cannot schedule reboot");
    }
    return send_json_text(request, "202 Accepted", "{\"rebooting\":true}");
}

static esp_err_t ota_handler(httpd_req_t *request)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || request->content_len <= 0 ||
        (size_t)request->content_len > partition->size) {
        return send_error(request, "400 Bad Request", "firmware image size is invalid");
    }
    char reason[128];
    if (!request_authorize(request, "POST", "/api/v1/ota",
        (size_t)request->content_len, NULL, reason, sizeof(reason))) {
        return send_error(request, "401 Unauthorized", reason);
    }
    char expected_hash[FW_AUTH_HEX_SHA256_LEN + 1];
    if (!request_header(request, "X-Content-SHA256", expected_hash, sizeof(expected_hash))) {
        return send_error(request, "400 Bad Request", "missing content SHA-256");
    }

    esp_ota_handle_t ota = 0;
    esp_err_t result = esp_ota_begin(partition, (size_t)request->content_len, &ota);
    if (result != ESP_OK) {
        return send_error(request, "500 Internal Server Error", esp_err_to_name(result));
    }
    uint8_t *buffer = malloc(OTA_RECEIVE_BUFFER);
    if (!buffer) {
        esp_ota_abort(ota);
        return send_error(request, "500 Internal Server Error", "out of memory");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        free(buffer);
        esp_ota_abort(ota);
        mbedtls_sha256_free(&sha);
        return send_error(request, "500 Internal Server Error", "SHA-256 initialization failed");
    }

    size_t remaining = (size_t)request->content_len;
    int64_t deadline_us = esp_timer_get_time() + OTA_RECEIVE_DEADLINE_US;
    while (remaining) {
        if (esp_timer_get_time() >= deadline_us) {
            result = ESP_ERR_TIMEOUT;
            break;
        }
        size_t wanted = remaining < OTA_RECEIVE_BUFFER ? remaining : OTA_RECEIVE_BUFFER;
        int received = httpd_req_recv(request, (char *)buffer, wanted);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0 || mbedtls_sha256_update(&sha, buffer, (size_t)received) != 0 ||
            esp_ota_write(ota, buffer, (size_t)received) != ESP_OK) {
            result = ESP_FAIL;
            break;
        }
        remaining -= (size_t)received;
    }
    uint8_t digest[32] = {0};
    if (result == ESP_OK && mbedtls_sha256_finish(&sha, digest) != 0) {
        result = ESP_FAIL;
    }
    mbedtls_sha256_free(&sha);
    free(buffer);

    char actual_hash[FW_AUTH_HEX_SHA256_LEN + 1];
    auth_hex_encode(digest, sizeof(digest), actual_hash);
    if (result != ESP_OK || remaining != 0 || strcasecmp(actual_hash, expected_hash) != 0) {
        esp_ota_abort(ota);
        return send_error(request, "400 Bad Request", "firmware transfer or SHA-256 validation failed");
    }
    result = esp_ota_end(ota);
    if (result != ESP_OK) {
        return send_error(request, "400 Bad Request", esp_err_to_name(result));
    }

    esp_app_desc_t candidate = {0};
    const esp_app_desc_t *running = esp_app_get_description();
    result = esp_ota_get_partition_description(partition, &candidate);
    if (result != ESP_OK || !running ||
        memcmp(candidate.project_name, running->project_name,
            sizeof(candidate.project_name)) != 0) {
        ESP_LOGW(TAG, "Rejected OTA image for a different ESP-IDF project");
        return send_error(request, "400 Bad Request",
            "firmware project identity does not match this device");
    }
    result = esp_ota_set_boot_partition(partition);
    if (result != ESP_OK) {
        return send_error(request, "400 Bad Request", esp_err_to_name(result));
    }

    ESP_LOGW(TAG, "OTA image accepted into %s, SHA-256 %s", partition->label, actual_hash);
    if (xTaskCreate(delayed_restart_task, "ota_restart", 2048,
        NULL, 4, NULL) != pdPASS) {
        return send_error(request, "500 Internal Server Error", "cannot schedule reboot");
    }
    return send_json_text(request, "202 Accepted",
        "{\"accepted\":true,\"verified\":true,\"rebooting\":true}");
}

esp_err_t web_admin_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = FW_HTTP_PORT;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &config), TAG, "start HTTP server");

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/v1/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/v1/auth/challenge", .method = HTTP_GET, .handler = challenge_handler},
        {.uri = "/api/v1/config", .method = HTTP_GET, .handler = config_get_handler},
        {.uri = "/api/v1/config", .method = HTTP_PUT, .handler = config_put_handler},
        {.uri = "/api/v1/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/v1/ota", .method = HTTP_POST, .handler = ota_handler},
    };
    for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_server, &handlers[i]),
            TAG, "register HTTP handler");
    }
    ESP_LOGI(TAG, "Management API listening on TCP %u", FW_HTTP_PORT);
    return ESP_OK;
}
