/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "time_config.h"
#include "config_model.h"
#include "freertos/semphr.h"
#include "nvs.h"

_Static_assert(sizeof(firmware_config_t) == 1052, "time settings must not alter base config ABI");

typedef struct {
    bool used;
    char name[16], key[16];
    uint8_t data[2048];
    size_t length;
} entry_t;
static entry_t entries[8], pending;
static char opened_name[16];
static bool opened, writable, fail_get, fail_set, fail_commit, fail_mutex, fail_create, fail_open;
static unsigned blob_sets, commits;
struct test_mutex { bool held; };
static struct test_mutex mutex;

static entry_t *entry(const char *name, const char *key, bool create)
{
    entry_t *empty = NULL;
    for (unsigned i = 0; i < sizeof(entries) / sizeof(entries[0]); ++i) {
        if (!entries[i].used) { if (!empty) empty = &entries[i]; }
        else if (!strcmp(entries[i].name, name) && !strcmp(entries[i].key, key)) return &entries[i];
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
    entry_t *saved = entry(name, key, true);
    assert(length <= sizeof(saved->data));
    if (length) memcpy(saved->data, data, length);
    saved->length = length;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return fail_create ? NULL : &mutex; }
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
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{
    if (fail_open) { fail_open = false; return ESP_FAIL; }
    assert(!opened && strlen(name) < sizeof(opened_name));
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
    entry_t *saved = entry(opened_name, key, false);
    if (!saved) return ESP_ERR_NVS_NOT_FOUND;
    if (data && *length < saved->length) { *length = saved->length; return ESP_ERR_NVS_INVALID_LENGTH; }
    if (data && saved->length) memcpy(data, saved->data, saved->length);
    *length = saved->length;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{
    assert(handle == 1 && opened && writable && !pending.used);
    ++blob_sets;
    if (fail_set) { fail_set = false; return ESP_FAIL; }
    assert(strlen(key) < sizeof(pending.key) && length <= sizeof(pending.data));
    pending.used = true;
    strcpy(pending.name, opened_name);
    strcpy(pending.key, key);
    memcpy(pending.data, data, length);
    pending.length = length;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1 && opened && writable);
    ++commits;
    if (fail_commit) { fail_commit = false; return ESP_FAIL; }
    if (pending.used) { seed(pending.name, pending.key, pending.data, pending.length); pending.used = false; }
    return ESP_OK;
}

static time_config_t candidate(void)
{
    time_config_t config = {.ntp_server = "192.168.1.20", .timezone = "PST8PDT,M3.2.0/2,M11.1.0/2"};
    return config;
}
static void assert_config(const time_config_t *expected)
{
    time_config_t actual;
    memset(&actual, 0xa5, sizeof(actual));
    time_config_get(&actual);
    assert(!strcmp(actual.ntp_server, expected->ntp_server));
    assert(!strcmp(actual.timezone, expected->timezone));
}
static void assert_defaults(void)
{
    time_config_t defaults = {.ntp_server = "pool.ntp.org", .timezone = "UTC0"};
    assert_config(&defaults);
}
static void setup(void) { assert(time_config_init() == ESP_OK); }

static void validation(void)
{
    time_config_t config = candidate();
    char reason[160];
    const char *valid_servers[] = {"pool.ntp.org", "ntp", "time-server.example.",
        "0.pool.ntp.org", "192.168.1.20", "1.2.3.4", "223.255.255.255"};
    const char *invalid_servers[] = {"", ".", "-ntp", "ntp-", "ntp..org", ".ntp",
        "ntp-.org", "ntp.-org", "ntp_server", "http://pool.ntp.org", "ntp:123", " ntp",
        "ntp\n", "::1", "[::1]", "1.2.3", "1.2.3.4.5", "1.2.3.256", "1.2.3.-1",
        "01.2.3.4", "0.0.0.0", "224.0.0.1", "255.255.255.255", "123", "1.2.3.4."};
    for (unsigned i = 0; i < sizeof(valid_servers) / sizeof(*valid_servers); ++i) {
        strcpy(config.ntp_server, valid_servers[i]);
        assert(time_config_validate(&config, reason, sizeof(reason)) && !reason[0]);
    }
    for (unsigned i = 0; i < sizeof(invalid_servers) / sizeof(*invalid_servers); ++i) {
        strcpy(config.ntp_server, invalid_servers[i]);
        assert(!time_config_validate(&config, reason, sizeof(reason)) && reason[0]);
    }
    config = candidate();
    const char *valid_zones[] = {"UTC0", "GMT+00:00", "IST-5:30", "ABC24", "ABC-24",
        "<+0530>-5:30", "ABCDEFGHIJ0", "PST8PDT,M3.2.0/2,M11.1.0/2",
        "EST5EDT4,J60/0,J300/24", "STD0DST-1,59/2:00:30,300/3:30:45",
        "ABC0DEF-1,M1.1.0,M12.5.6", "ABC0:00:00DEF-1:00:00,J1,J365"};
    const char *invalid_zones[] = {"", "UTC", "U0", "UT0", "ABCDEFGHIJK0", "<AB>0",
        "<ABCDEFGHIJK>0", "<UTC0", "<UT_C>0", "UTC0 ", " UTC0", "America/Los_Angeles",
        ":UTC0", "UTC25", "UTC-25", "UTC24:01", "UTC24:00:01", "UTC0:60",
        "UTC0:0:60", "UTC0:00:01", "UTC0:0:", "UTC000", "UTC0DST",
        "UTC0DST-1:00:01,M3.2.0,M11.1.0", "UTC0DST,", "UTC0DST,M3.2.0",
        "UTC0DST,M0.2.0,M11.1.0", "UTC0DST,M13.2.0,M11.1.0", "UTC0DST,M3.0.0,M11.1.0",
        "UTC0DST,M3.6.0,M11.1.0", "UTC0DST,M3.2.7,M11.1.0", "UTC0DST,J0,J300",
        "UTC0DST,J366,J300", "UTC0DST,366,300", "UTC0DST,59/-1,300",
        "UTC0DST,59/25,300", "UTC0DST,59/24:00:01,300", "UTC0DST,59,300garbage",
        "UTC0DST,59,300,301", "UTC0\n"};
    for (unsigned i = 0; i < sizeof(valid_zones) / sizeof(*valid_zones); ++i) {
        strcpy(config.timezone, valid_zones[i]);
        assert(time_config_validate(&config, reason, sizeof(reason)) && !reason[0]);
    }
    for (unsigned i = 0; i < sizeof(invalid_zones) / sizeof(*invalid_zones); ++i) {
        strcpy(config.timezone, invalid_zones[i]);
        assert(!time_config_validate(&config, reason, sizeof(reason)) && reason[0]);
    }
    config = candidate();
    memset(config.ntp_server, 'a', sizeof(config.ntp_server));
    assert(!time_config_validate(&config, NULL, 0));
    config = candidate();
    memset(config.timezone, 'a', sizeof(config.timezone));
    assert(!time_config_validate(&config, NULL, 0));
    assert(!time_config_validate(NULL, reason, 1) && !reason[0]);
}

static void defaults(void)
{
    assert_defaults(); /* usable even before optional initialization */
    setup();
    assert_defaults();
    assert(!blob_sets && !commits && !entry("bacnet_cfg", "time_config", false));
    time_config_t config;
    time_config_get(&config);
    assert(time_config_update(&config) == ESP_OK);
    assert(blob_sets == 1 && commits == 1); /* explicit save persists defaults */
}
static void roundtrip(void)
{
    setup();
    time_config_t config = candidate();
    assert(time_config_update(&config) == ESP_OK);
    assert_config(&config);
    entry_t *saved = entry("bacnet_cfg", "time_config", false);
    assert(saved && saved->data[0] == 1 && saved->data[1] == strlen(config.ntp_server));
    assert(saved->data[2] == 0 && saved->data[3] == strlen(config.timezone));
    assert(saved->length == 4U + strlen(config.ntp_server) + strlen(config.timezone));
    assert(time_config_init() == ESP_OK);
    assert_config(&config);
    assert(blob_sets == 1 && commits == 1);
}
static void identical(void)
{
    setup();
    time_config_t config = candidate();
    assert(time_config_update(&config) == ESP_OK);
    memset(config.ntp_server + strlen(config.ntp_server) + 1U, 0xa5,
        sizeof(config.ntp_server) - strlen(config.ntp_server) - 1U);
    assert(time_config_update(&config) == ESP_OK);
    assert(blob_sets == 1 && commits == 1); /* caller padding is not persisted */
}
static void bounds(void)
{
    setup();
    time_config_t config = candidate();
    memset(config.ntp_server, 'a', 253);
    config.ntp_server[63] = config.ntp_server[127] = config.ntp_server[191] = '.';
    config.ntp_server[253] = '\0';
    assert(time_config_validate(&config, NULL, 0));
    assert(time_config_update(&config) == ESP_OK);
    assert(time_config_init() == ESP_OK);
    assert_config(&config);
    config.ntp_server[63] = 'a';
    assert(!time_config_validate(&config, NULL, 0));
    assert(time_config_update(&config) == ESP_ERR_INVALID_ARG);
}
static void corrupt(void)
{
    setup();
    const uint8_t invalid[][20] = {
        {2, 3, 0, 4, 'n','t','p','U','T','C','0'},
        {1, 3, 0, 4, 'n','t','p','U','T','C','1','0'},
        {1, 255, 255, 4}, {1, 0, 0, 0},
        {1, 3, 0, 4, 'n',0,'p','U','T','C','0'},
        {1, 3, 0, 4, 'n','t','p','U','T','C','X'},
    };
    const size_t lengths[] = {11,12,4,4,11,11};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        seed("bacnet_cfg", "time_config", invalid[i], lengths[i]);
        assert(time_config_init() == ESP_OK);
        assert_defaults();
    }
    uint8_t oversized[400] = {0};
    seed("bacnet_cfg", "time_config", oversized, sizeof(oversized));
    assert(time_config_init() == ESP_OK);
    assert_defaults();
    assert(!blob_sets && !commits); /* corruption never overwrites evidence */
}
static void failure(const char *which)
{
    setup();
    time_config_t original = candidate();
    assert(time_config_update(&original) == ESP_OK);
    time_config_t replacement = original;
    strcpy(replacement.ntp_server, "other.example");
    if (!strcmp(which, "set-error")) fail_set = true;
    if (!strcmp(which, "commit-error")) fail_commit = true;
    if (!strcmp(which, "open-error")) fail_open = true;
    if (!strcmp(which, "mutex-error")) fail_mutex = true;
    esp_err_t result = time_config_update(&replacement);
    assert(result == (!strcmp(which, "mutex-error") ? ESP_ERR_TIMEOUT : ESP_FAIL));
    assert_config(&original);
    assert(time_config_init() == ESP_OK);
    assert_config(&original);
}
static void read_error(void)
{
    setup();
    time_config_t original = candidate();
    assert(time_config_update(&original) == ESP_OK);
    fail_get = true;
    assert(time_config_init() == ESP_FAIL);
    assert_config(&original);
    fail_open = true;
    assert(time_config_init() == ESP_FAIL);
    assert_config(&original);
}
static void preservation(void)
{
    uint8_t original[1052], secret[32], runtime[4], recipients[3];
    memset(original, 0x41, sizeof(original));
    memset(secret, 0xa5, sizeof(secret));
    memset(runtime, 0x7a, sizeof(runtime));
    memset(recipients, 0x19, sizeof(recipients));
    seed("bacnet_cfg", "config", original, sizeof(original));
    seed("bacnet_sec", "admin_key", secret, sizeof(secret));
    seed("bacnet_run", "boots", runtime, sizeof(runtime));
    seed("bacnet_cfg", "restart_rcpt", recipients, sizeof(recipients));
    setup();
    time_config_t config = candidate();
    assert(time_config_update(&config) == ESP_OK);
    assert(time_config_init() == ESP_OK);
    assert(!memcmp(entry("bacnet_cfg", "config", false)->data, original, sizeof(original)));
    assert(!memcmp(entry("bacnet_sec", "admin_key", false)->data, secret, sizeof(secret)));
    assert(!memcmp(entry("bacnet_run", "boots", false)->data, runtime, sizeof(runtime)));
    assert(!memcmp(entry("bacnet_cfg", "restart_rcpt", false)->data, recipients, sizeof(recipients)));
}
static void no_memory(void)
{
    fail_create = true;
    assert(time_config_init() == ESP_ERR_NO_MEM);
    assert_defaults();
    time_config_t config = candidate();
    assert(time_config_update(&config) == ESP_ERR_INVALID_ARG);
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "validation")) validation();
    else if (!strcmp(argv[1], "defaults")) defaults();
    else if (!strcmp(argv[1], "roundtrip")) roundtrip();
    else if (!strcmp(argv[1], "identical")) identical();
    else if (!strcmp(argv[1], "bounds")) bounds();
    else if (!strcmp(argv[1], "corrupt")) corrupt();
    else if (!strcmp(argv[1], "read-error")) read_error();
    else if (!strcmp(argv[1], "preservation")) preservation();
    else if (!strcmp(argv[1], "no-memory")) no_memory();
    else if (!strcmp(argv[1], "preset")) {
        assert(argc == 3 && strlen(argv[2]) < TIME_CONFIG_TIMEZONE_SIZE);
        time_config_t config = candidate();
        strcpy(config.timezone, argv[2]);
        assert(time_config_validate(&config, NULL, 0));
    } else failure(argv[1]);
    printf("time config: %s passed\n", argv[1]);
    return 0;
}
