/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "config_store.h"
#include "freertos/semphr.h"
#include "nvs.h"

/* Fake NVS commits one staged value atomically. This exercises API failures,
   not ESP flash power-loss guarantees. No credentials or device are used. */
typedef struct {
    bool used;
    char name[16], key[16];
    uint8_t data[2048];
    size_t length;
} entry_t;
static entry_t entries[8], pending;
static char opened_name[16];
static bool opened, writable, fail_get, fail_set, fail_commit, fail_mutex;
static unsigned blob_sets, commits;
struct test_mutex { bool held; };
static struct test_mutex mutex;
static firmware_config_t original_config;
static const uint8_t recipient_a[] = {0x0c, 0x02, 0x00, 0x01, 0xfb};
static const uint8_t recipient_b[] = {0x0c, 0x02, 0x00, 0x01, 0xfc};

static entry_t *entry(const char *name, const char *key, bool create)
{
    entry_t *empty = NULL;
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        if (!entries[i].used) {
            if (!empty) empty = &entries[i];
        } else if (!strcmp(entries[i].name, name) && !strcmp(entries[i].key, key)) {
            return &entries[i];
        }
    }
    if (!create) return NULL;
    assert(empty && strlen(name) < sizeof(empty->name) && strlen(key) < sizeof(empty->key));
    empty->used = true;
    strcpy(empty->name, name);
    strcpy(empty->key, key);
    return empty;
}

static void seed(const char *name, const char *key, const void *data, size_t length)
{
    entry_t *value = entry(name, key, true);
    assert(length <= sizeof(value->data));
    if (length) memcpy(value->data, data, length);
    value->length = length;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    assert(handle == &mutex && !mutex.held && ticks == portMAX_DELAY);
    if (fail_mutex) { fail_mutex = false; return pdFALSE; }
    mutex.held = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle == &mutex && mutex.held);
    mutex.held = false;
    return pdTRUE;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
void esp_fill_random(void *buffer, size_t length) { memset(buffer, 0x9b, length); }

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    assert(!opened && name && handle && strlen(name) < sizeof(opened_name));
    opened = true;
    writable = mode == NVS_READWRITE;
    strcpy(opened_name, name);
    *handle = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1 && opened);
    opened = false;
    pending.used = false;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *data, size_t *length)
{
    assert(handle == 1 && opened && length);
    if (fail_get) { fail_get = false; return ESP_FAIL; }
    entry_t *value = entry(opened_name, key, false);
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    if (data && *length < value->length) {
        *length = value->length;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    if (data && value->length) memcpy(data, value->data, value->length);
    *length = value->length;
    return ESP_OK;
}
static esp_err_t stage(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle == 1 && opened && writable && !pending.used);
    if (fail_set) { fail_set = false; return ESP_FAIL; }
    assert(strlen(key) < sizeof(pending.key) && length <= sizeof(pending.data));
    pending.used = true;
    strcpy(pending.name, opened_name);
    strcpy(pending.key, key);
    memcpy(pending.data, data, length);
    pending.length = length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    blob_sets++;
    return stage(handle, key, data, length);
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && opened && writable);
    commits++;
    if (fail_commit) { fail_commit = false; return ESP_FAIL; }
    if (pending.used) {
        seed(pending.name, pending.key, pending.data, pending.length);
        pending.used = false;
    }
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *value)
{
    size_t length = sizeof(*value);
    return nvs_get_blob(handle, key, value, &length);
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value)
{
    return stage(handle, key, &value, sizeof(value));
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    size_t length = sizeof(*value);
    return nvs_get_blob(handle, key, value, &length);
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    return stage(handle, key, &value, sizeof(value));
}

static void setup(void)
{
    config_model_defaults(&original_config);
    original_config.device_instance = 600001;
    original_config.device_instance_mode = FW_INSTANCE_MANUAL;
    strcpy(original_config.device_name, "Preserved controller identity");
    strcpy(original_config.hostname, "stable-controller");
    config_model_finalize(&original_config);
    assert(sizeof(original_config) == 1052 && config_model_is_valid_blob(&original_config));
    seed("bacnet_cfg", "config", &original_config, sizeof(original_config));
    const uint8_t fake_key[32] = {0xa5};
    const uint8_t relays = 0x5a;
    const uint32_t boots = 9;
    seed("bacnet_sec", "admin_key", fake_key, sizeof(fake_key));
    seed("bacnet_run", "relays", &relays, sizeof(relays));
    seed("bacnet_run", "boots", &boots, sizeof(boots));
    assert(config_store_init() == ESP_OK);
    assert(config_store_reboot_count() == 10);
    blob_sets = commits = 0;
}

static void assert_base_unchanged(void)
{
    entry_t *base = entry("bacnet_cfg", "config", false);
    assert(base && base->length == sizeof(original_config));
    assert(!memcmp(base->data, &original_config, sizeof(original_config)));
    firmware_config_t current;
    config_store_get(&current);
    assert(!memcmp(&current, &original_config, sizeof(current)));
    assert(entry("bacnet_sec", "admin_key", false)->data[0] == 0xa5);
    assert(entry("bacnet_run", "relays", false)->data[0] == 0x5a);
    uint32_t boots;
    memcpy(&boots, entry("bacnet_run", "boots", false)->data, sizeof(boots));
    assert(boots == 10);
}

static void assert_payload(const uint8_t *expected, size_t expected_length)
{
    uint8_t actual[CONFIG_STORE_RESTART_RECIPIENTS_MAX_BYTES];
    memset(actual, 0xcc, sizeof(actual));
    size_t length = 999;
    assert(config_store_restart_recipients_get(actual, sizeof(actual), &length) == ESP_OK);
    assert(length == expected_length);
    if (length) assert(!memcmp(actual, expected, length));
    if (length < sizeof(actual)) assert(actual[length] == 0xcc);
}

static void assert_get_failure(esp_err_t expected)
{
    uint8_t actual[256];
    memset(actual, 0xcc, sizeof(actual));
    size_t length = 999;
    assert(config_store_restart_recipients_get(actual, sizeof(actual), &length) == expected);
    assert(length == 0);
    for (unsigned i = 0; i < sizeof(actual); i++) assert(actual[i] == 0xcc);
}

static void missing(void)
{
    assert_get_failure(ESP_ERR_NVS_NOT_FOUND);
    assert(blob_sets == 0 && commits == 0);
}
static void empty(void)
{
    assert(config_store_restart_recipients_set(NULL, 0) == ESP_OK);
    assert_payload(NULL, 0);
    entry_t *saved = entry("bacnet_cfg", "restart_rcpt", false);
    assert(saved && saved->length == 3 && saved->data[0] == 1);
    assert(saved->data[1] == 0 && saved->data[2] == 0);
    size_t length = 999;
    assert(config_store_restart_recipients_get(NULL, 0, &length) == ESP_OK && length == 0);
    assert(config_store_restart_recipients_set(NULL, 0) == ESP_OK);
    assert(blob_sets == 1 && commits == 1);
}
static void roundtrip(void)
{
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    assert_payload(recipient_a, sizeof(recipient_a));
    assert(config_store_restart_recipients_set(recipient_b, sizeof(recipient_b)) == ESP_OK);
    assert_payload(recipient_b, sizeof(recipient_b));
    assert(blob_sets == 2 && commits == 2);
}
static void identical(void)
{
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    assert_payload(recipient_a, sizeof(recipient_a));
    assert(blob_sets == 1 && commits == 1);
}
static void bounds(void)
{
    uint8_t payload[257];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = (uint8_t)i;
    assert(config_store_restart_recipients_set(payload, 257) == ESP_ERR_INVALID_SIZE);
    assert(config_store_restart_recipients_set(NULL, 1) == ESP_ERR_INVALID_ARG);
    assert(blob_sets == 0 && commits == 0);
    assert(config_store_restart_recipients_set(payload, 256) == ESP_OK);
    assert_payload(payload, 256);
    entry_t *saved = entry("bacnet_cfg", "restart_rcpt", false);
    assert(saved->length == 259 && saved->data[1] == 0 && saved->data[2] == 1);
    size_t length = 999;
    uint8_t destination[255];
    memset(destination, 0xcc, sizeof(destination));
    assert(config_store_restart_recipients_get(destination, sizeof(destination), &length) == ESP_ERR_INVALID_SIZE);
    assert(length == 0 && destination[0] == 0xcc);
    assert(config_store_restart_recipients_get(NULL, 1, &length) == ESP_ERR_INVALID_ARG);
    assert(config_store_restart_recipients_get(destination, sizeof(destination), NULL) == ESP_ERR_INVALID_ARG);
}
static void corrupt(void)
{
    const uint8_t bad_version[] = {2, 0, 0};
    const uint8_t short_header[] = {1, 0};
    const uint8_t wrong_length[] = {1, 4, 0, 0x0c};
    seed("bacnet_cfg", "restart_rcpt", bad_version, sizeof(bad_version));
    assert_get_failure(ESP_ERR_INVALID_STATE);
    seed("bacnet_cfg", "restart_rcpt", short_header, sizeof(short_header));
    assert_get_failure(ESP_ERR_INVALID_STATE);
    seed("bacnet_cfg", "restart_rcpt", wrong_length, sizeof(wrong_length));
    assert_get_failure(ESP_ERR_INVALID_STATE);
    uint8_t oversized[260] = {1, 1, 1};
    seed("bacnet_cfg", "restart_rcpt", oversized, sizeof(oversized));
    assert_get_failure(ESP_ERR_NVS_INVALID_LENGTH);
    assert(blob_sets == 0 && commits == 0);
    /* Only an explicit valid write replaces invalid optional storage. */
    assert(config_store_restart_recipients_set(NULL, 0) == ESP_OK);
    assert_payload(NULL, 0);
}
static void read_error(void)
{
    fail_get = true;
    assert_get_failure(ESP_FAIL);
    fail_get = true;
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_FAIL);
    assert(blob_sets == 0 && commits == 0);
}
static void set_error(void)
{
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    fail_set = true;
    assert(config_store_restart_recipients_set(recipient_b, sizeof(recipient_b)) == ESP_FAIL);
    assert_payload(recipient_a, sizeof(recipient_a));
    assert(blob_sets == 2 && commits == 1);
}
static void commit_error(void)
{
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    uint8_t caller_cache[sizeof(recipient_a)];
    memcpy(caller_cache, recipient_a, sizeof(caller_cache));
    fail_commit = true;
    esp_err_t result = config_store_restart_recipients_set(recipient_b, sizeof(recipient_b));
    if (result == ESP_OK) memcpy(caller_cache, recipient_b, sizeof(caller_cache));
    assert(result == ESP_FAIL && !memcmp(caller_cache, recipient_a, sizeof(caller_cache)));
    assert_payload(recipient_a, sizeof(recipient_a));
    assert(blob_sets == 2 && commits == 2);
}
static void preservation(void)
{
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_OK);
    assert_base_unchanged();
    /* Existing/rollback code writes only its known base-config key. */
    firmware_config_t updated = original_config;
    strcpy(updated.location, "Legacy configuration update");
    assert(config_store_update(&updated) == ESP_OK);
    assert_payload(recipient_a, sizeof(recipient_a));
    config_store_get(&original_config);
    assert(original_config.device_instance == 600001);
    assert(original_config.device_instance_mode == FW_INSTANCE_MANUAL);
    assert(config_model_is_valid_blob(&original_config) && sizeof(original_config) == 1052);
}
static void mutex_error(void)
{
    fail_mutex = true;
    assert_get_failure(ESP_ERR_TIMEOUT);
    fail_mutex = true;
    assert(config_store_restart_recipients_set(recipient_a, sizeof(recipient_a)) == ESP_ERR_TIMEOUT);
    assert(blob_sets == 0 && commits == 0);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    setup();
    if (!strcmp(argv[1], "missing")) missing();
    else if (!strcmp(argv[1], "empty")) empty();
    else if (!strcmp(argv[1], "roundtrip")) roundtrip();
    else if (!strcmp(argv[1], "identical")) identical();
    else if (!strcmp(argv[1], "bounds")) bounds();
    else if (!strcmp(argv[1], "corrupt")) corrupt();
    else if (!strcmp(argv[1], "read-error")) read_error();
    else if (!strcmp(argv[1], "set-error")) set_error();
    else if (!strcmp(argv[1], "commit-error")) commit_error();
    else if (!strcmp(argv[1], "preservation")) preservation();
    else if (!strcmp(argv[1], "mutex-error")) mutex_error();
    else assert(!"unknown scenario");
    assert_base_unchanged();
    assert(!opened && !mutex.held);
    printf("PASS restart recipient store %s\n", argv[1]);
    return 0;
}
