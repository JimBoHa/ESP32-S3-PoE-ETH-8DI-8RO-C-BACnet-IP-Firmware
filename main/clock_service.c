/* SPDX-License-Identifier: 0BSD */
#include "clock_service.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/ip_addr.h"
#include "clock_model.h"
#include "ethernet_manager.h"
#include "time_config.h"

#define RETAINED_CLOCK_MAGIC UINT32_C(0x4e545031) /* NTP1 */
static const char *TAG = "clock";
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t s_timezone_mutex;
static TaskHandle_t s_task;
static clock_model_t s_model;
static bool s_sntp_running;
static uint32_t s_start_failures;
static uint32_t s_requested_generation = 1;
static uint32_t s_applied_generation;
/* Hostname belongs to the owner task; SNTP receives binary addresses only. */
static time_config_t s_runtime_config;
typedef struct { uint32_t magic; int64_t last_sync_us; uint32_t crc; } retained_t;
static RTC_NOINIT_ATTR volatile retained_t s_retained;

static const char *clock_source(const clock_model_t *m)
{
    return !m->valid ? "unsynchronized" : m->network_sync ? "ntp" : "retained-ntp";
}

/* Documented ESP-IDF weak override: reject invalid epochs BEFORE setting POSIX
   time. The esp-netif wrapper's optional callback/wait semaphore are not used.
   Runs on TCP/IP task: no mutex waits, BACnet calls, flash, or network work. */
void sntp_sync_time(struct timeval *tv)
{
    int64_t uptime = esp_timer_get_time();
    if (!tv || !clock_model_time_valid(tv->tv_sec, tv->tv_usec) || uptime < 0 ||
        uptime > (int64_t)tv->tv_sec * 1000000 + tv->tv_usec -
            CLOCK_MIN_UNIX_SECONDS * INT64_C(1000000)) {
        portENTER_CRITICAL(&s_state_lock);
        s_model.rejected_syncs++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    if (settimeofday(tv, NULL) != 0) {
        portENTER_CRITICAL(&s_state_lock);
        s_model.rejected_syncs++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    int64_t unix_us = (int64_t)tv->tv_sec * 1000000 + tv->tv_usec;
    portENTER_CRITICAL(&s_state_lock);
    (void)clock_model_accept(&s_model, tv->tv_sec, tv->tv_usec, uptime, true);
    s_retained.magic = 0;
    s_retained.last_sync_us = unix_us;
    s_retained.crc = clock_model_retained_checksum(unix_us);
    s_retained.magic = RETAINED_CLOCK_MAGIC;
    portEXIT_CRITICAL(&s_state_lock);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
}

static bool local_snapshot(const clock_model_t *model, int64_t uptime,
    clock_service_snapshot_t *out, clock_service_status_t *status)
{
    memset(out, 0, sizeof(*out));
    out->source = clock_source(model);
    if (!clock_model_sample(model, uptime, &out->utc_unix_us, &out->boot_utc_unix_us))
        return true;
    if (!s_timezone_mutex || xSemaphoreTake(s_timezone_mutex, pdMS_TO_TICKS(10)) != pdTRUE)
        return false;
    time_t current = (time_t)(out->utc_unix_us / 1000000);
    time_t boot = (time_t)(out->boot_utc_unix_us / 1000000);
    bool converted = localtime_r(&current, &out->local_time) &&
        localtime_r(&boot, &out->boot_local_time);
    /* newlib's _timezone is seconds WEST for standard time, not tm_gmtoff
       which includes seasonal DST. It is refreshed by tzset(). */
    extern long _timezone;
    long offset = _timezone / 60;
    out->utc_offset_minutes = (int16_t)offset;
    out->daylight_saving = out->local_time.tm_isdst > 0;
    out->valid = converted && offset >= -1440 && offset <= 1440 &&
        out->local_time.tm_year >= 120 && out->local_time.tm_year < 200 &&
        out->boot_local_time.tm_year >= 120 && out->boot_local_time.tm_year < 200;
    if (out->valid && status) {
        struct tm utc_tm;
        if (gmtime_r(&current, &utc_tm))
            strftime(status->utc_time, sizeof(status->utc_time), "%Y-%m-%dT%H:%M:%SZ", &utc_tm);
        /* newlib strftime may consult TZ state: keep this in the same lock. */
        strftime(status->local_time, sizeof(status->local_time), "%Y-%m-%dT%H:%M:%S%z", &out->local_time);
    }
    xSemaphoreGive(s_timezone_mutex);
    return true;
}

bool clock_service_snapshot_get(clock_service_snapshot_t *out)
{
    if (!out) return false;
    clock_model_t model;
    portENTER_CRITICAL(&s_state_lock);
    model = s_model;
    portEXIT_CRITICAL(&s_state_lock);
    return local_snapshot(&model, esp_timer_get_time(), out, NULL);
}

bool clock_service_status_get(clock_service_status_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    clock_model_t model;
    portENTER_CRITICAL(&s_state_lock);
    model = s_model;
    out->sntp_running = s_sntp_running;
    out->start_failures = s_start_failures;
    out->config_generation = s_applied_generation;
    portEXIT_CRITICAL(&s_state_lock);
    int64_t uptime = esp_timer_get_time();
    clock_service_snapshot_t snap;
    if (!local_snapshot(&model, uptime, &snap, out)) return false;
    out->valid = snap.valid;
    out->synchronized = model.network_sync;
    out->source = clock_source(&model);
    out->sync_count = model.sync_count;
    out->rejected_syncs = model.rejected_syncs;
    out->last_sync_unix_us = model.last_sync_unix_us;
    out->last_sync_uptime_ms = (uint64_t)model.last_sync_uptime_us / 1000;
    if (snap.valid) {
        if (model.last_sync_unix_us && snap.utc_unix_us >= model.last_sync_unix_us)
            out->sync_age_seconds = (uint64_t)(snap.utc_unix_us - model.last_sync_unix_us) / 1000000;
    }
    return true;
}

void clock_service_config_changed(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_requested_generation++;
    portEXIT_CRITICAL(&s_state_lock);
    if (s_task) xTaskNotifyGive(s_task);
}

static bool resolve_server(const char *name, ip_addr_t *address)
{
    const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM };
    struct addrinfo *answer = NULL;
    int result = getaddrinfo(name, NULL, &hints, &answer);
    bool valid = result == 0 && answer && answer->ai_family == AF_INET &&
        answer->ai_addr && answer->ai_addrlen >= sizeof(struct sockaddr_in);
    if (valid) {
        uint32_t ipv4 = ((const struct sockaddr_in *)answer->ai_addr)->sin_addr.s_addr;
        uint32_t host = ntohl(ipv4);
        valid = (host >> 24) != 0 && (host >> 24) < 224;
        if (valid) ip_addr_set_ip4_u32(address, ipv4);
    }
    if (answer) freeaddrinfo(answer);
    return valid;
}

static void clock_task(void *unused)
{
    (void)unused;
    bool initialized = false;
    bool last_has_ip = false;
    uint32_t last_network_revision = 0;
    uint32_t configured = 0;
    int64_t retry_after = 0;
    int64_t init_retry_after = 0;
    for (;;) {
        uint32_t requested;
        portENTER_CRITICAL(&s_state_lock);
        requested = s_requested_generation;
        portEXIT_CRITICAL(&s_state_lock);
        bool has_ip = ethernet_manager_has_ip();
        uint32_t revision = ethernet_manager_network_revision();
        int64_t now = esp_timer_get_time();
        if (!initialized && now >= init_retry_after) {
            esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(NULL);
            config.num_of_servers = 0;
            config.start = false;
            config.wait_for_sync = false;
            config.sync_cb = NULL;
            initialized = esp_netif_sntp_init(&config) == ESP_OK;
            if (!initialized) {
                portENTER_CRITICAL(&s_state_lock);
                s_start_failures++;
                portEXIT_CRITICAL(&s_state_lock);
                init_retry_after = now + INT64_C(15000000);
            }
        }
        if (requested != configured) {
            if (initialized) esp_sntp_stop();
            time_config_get(&s_runtime_config);
            xSemaphoreTake(s_timezone_mutex, portMAX_DELAY);
            int tz_error = setenv("TZ", s_runtime_config.timezone, 1);
            if (!tz_error) tzset();
            xSemaphoreGive(s_timezone_mutex);
            configured = requested;
            portENTER_CRITICAL(&s_state_lock);
            s_applied_generation = configured;
            s_sntp_running = false;
            if (tz_error) s_start_failures++;
            portEXIT_CRITICAL(&s_state_lock);
            retry_after = now;
        }
        if (has_ip != last_has_ip || revision != last_network_revision) {
            if (initialized) esp_sntp_stop();
            portENTER_CRITICAL(&s_state_lock);
            s_sntp_running = false;
            portEXIT_CRITICAL(&s_state_lock);
            retry_after = now;
        }
        last_has_ip = has_ip;
        last_network_revision = revision;
        if (initialized && has_ip && now >= retry_after) {
            esp_sntp_stop();
            portENTER_CRITICAL(&s_state_lock);
            s_sntp_running = false;
            portEXIT_CRITICAL(&s_state_lock);
            /* Pinned lwIP does not cancel asynchronous SNTP DNS callbacks on
               stop. Resolve in this separate task, then install a BINARY IP:
               SNTP never owns an async DNS callback or a hostname pointer.
               Slow DNS cannot hold BACnet/relay/config mutexes. */
            ip_addr_t address = {0};
            bool resolved = resolve_server(s_runtime_config.ntp_server, &address);
            uint32_t latest_requested;
            portENTER_CRITICAL(&s_state_lock);
            latest_requested = s_requested_generation;
            portEXIT_CRITICAL(&s_state_lock);
            bool still_current = latest_requested == configured &&
                ethernet_manager_has_ip() && ethernet_manager_network_revision() == revision;
            if (still_current) {
                esp_err_t result = ESP_FAIL;
                if (resolved) {
                    esp_sntp_setserver(0, &address);
                    result = esp_netif_sntp_start();
                }
                bool started = result == ESP_OK;
                portENTER_CRITICAL(&s_state_lock);
                s_sntp_running = started;
                if (!started) s_start_failures++;
                portEXIT_CRITICAL(&s_state_lock);
                if (!started) ESP_LOGW(TAG, "NTP DNS/start failed; retry in 30 seconds");
                retry_after = esp_timer_get_time() + (started ? INT64_C(3600000000) : INT64_C(30000000));
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    }
}

esp_err_t clock_service_init(void)
{
    s_timezone_mutex = xSemaphoreCreateMutex();
    if (!s_timezone_mutex) return ESP_ERR_NO_MEM;
    time_config_get(&s_runtime_config);
    if (setenv("TZ", s_runtime_config.timezone, 1) != 0) return ESP_ERR_NO_MEM;
    tzset();
    esp_reset_reason_t reason = esp_reset_reason();
    struct timeval tv;
    int64_t last_sync = s_retained.last_sync_us;
    if ((reason == ESP_RST_SW || reason == ESP_RST_DEEPSLEEP) &&
        s_retained.magic == RETAINED_CLOCK_MAGIC &&
        s_retained.crc == clock_model_retained_checksum(last_sync) &&
        gettimeofday(&tv, NULL) == 0 && clock_model_time_valid(tv.tv_sec, tv.tv_usec) &&
        clock_model_holdover_valid((int64_t)tv.tv_sec * 1000000 + tv.tv_usec, last_sync)) {
        (void)clock_model_accept(&s_model, tv.tv_sec, tv.tv_usec, esp_timer_get_time(), false);
        s_model.last_sync_unix_us = last_sync;
    } else {
        s_retained.magic = 0;
    }
    if (xTaskCreate(clock_task, "ntp_clock", 4096, NULL, 3, &s_task) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
