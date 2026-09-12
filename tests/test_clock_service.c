/* SPDX-License-Identifier: 0BSD */
#include <assert.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "clock_service.h"
#include "clock_model.h"
#include "esp_system.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "time_config.h"

/* Include actual source, not a behavioral copy. Only host-sensitive libc
   calls are redirected. The model is compiled as its normal separate TU. */
static int clock_test_settimeofday(const struct timeval *, const struct timezone *);
static int clock_test_gettimeofday(struct timeval *, void *);
static void clock_test_tzset(void);
static size_t clock_test_strftime(char *, size_t, const char *, const struct tm *);
static int clock_test_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
static void clock_test_freeaddrinfo(struct addrinfo *);
static long clock_test_timezone;
#define settimeofday clock_test_settimeofday
#define gettimeofday clock_test_gettimeofday
#define tzset clock_test_tzset
#define ESP_PLATFORM 1
#define _timezone clock_test_timezone
#define strftime clock_test_strftime
#define getaddrinfo clock_test_getaddrinfo
#define freeaddrinfo clock_test_freeaddrinfo
#include "../main/clock_service.c"
#undef freeaddrinfo
#undef getaddrinfo
#undef strftime
#undef _timezone
#undef ESP_PLATFORM
#undef tzset
#undef gettimeofday
#undef settimeofday

struct clock_test_mutex { bool held; };
static struct clock_test_mutex mutex;
static int64_t uptime_us = 5000000;
static struct timeval wall = { 1782864000, 123456 }; /* 2026-07-01 UTC */
static esp_reset_reason_t reset_reason = ESP_RST_POWERON;
static bool fail_set, fail_get, fail_take, has_ip;
static unsigned set_calls, get_calls, completed, notifications;
static unsigned inits, deinits, starts, stops, server_sets, fail_inits, iterations;
static unsigned resolves, frees, fail_resolves;
static bool netif_initialized, sntp_enabled, dns_invalid_address;
static uint32_t network_revision;
static void (*owner_task)(void *);
static void (*after_iteration)(unsigned);
static void (*resolver_hook)(void);
static jmp_buf task_done;
static time_config_t saved_config = {
    .ntp_server = TIME_CONFIG_DEFAULT_NTP_SERVER,
    .timezone = TIME_CONFIG_DEFAULT_TIMEZONE,
};
static ip_addr_t active_address;
static char last_resolved_name[TIME_CONFIG_NTP_SERVER_SIZE];
static struct sockaddr_in dns_sockaddr;
static struct addrinfo dns_answer;

void clock_test_enter_critical(portMUX_TYPE *lock)
{
    assert(lock && !lock->entered);
    lock->entered++;
}
void clock_test_exit_critical(portMUX_TYPE *lock)
{
    assert(lock && lock->entered == 1);
    lock->entered--;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks)
{
    assert(handle == &mutex && !mutex.held && !s_state_lock.entered);
    assert(ticks == 10 || ticks == portMAX_DELAY);
    if (fail_take) { fail_take = false; return pdFALSE; }
    mutex.held = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle == &mutex && mutex.held);
    mutex.held = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
    void *argument, unsigned priority, TaskHandle_t *handle)
{
    assert(task && !argument && !strcmp(name, "ntp_clock") && stack >= 4096 && priority);
    owner_task = task;
    *handle = &mutex;
    return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t task)
{
    assert(task == &mutex);
    notifications++;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    assert(clear == pdTRUE && ticks == 250 && after_iteration);
    assert(!mutex.held && !s_state_lock.entered);
    after_iteration(++iterations);
    uptime_us += 250000;
    return 0;
}
int64_t esp_timer_get_time(void) { return uptime_us; }
esp_reset_reason_t esp_reset_reason(void) { return reset_reason; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
bool ethernet_manager_has_ip(void) { return has_ip; }
uint32_t ethernet_manager_network_revision(void) { return network_revision; }
void time_config_get(time_config_t *config) { *config = saved_config; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config)
{
    assert(config && !netif_initialized && !mutex.held && !s_state_lock.entered);
    assert(!config->wait_for_sync && !config->sync_cb && !config->start);
    assert(!config->smooth_sync && !config->server_from_dhcp);
    assert(!config->num_of_servers && !config->servers[0]);
    inits++;
    if (fail_inits) { fail_inits--; return ESP_FAIL; }
    netif_initialized = true;
    return ESP_OK;
}
void esp_netif_sntp_deinit(void)
{
    deinits++;
    assert(false && "owner must not deinitialize active SNTP");
}
esp_err_t esp_netif_sntp_start(void)
{
    assert(netif_initialized && server_sets && !sntp_enabled);
    assert(active_address.type == IPADDR_TYPE_V4 && active_address.u_addr.ip4.addr);
    assert(!mutex.held && !s_state_lock.entered);
    starts++;
    sntp_enabled = true;
    return ESP_OK;
}
void esp_sntp_stop(void)
{
    assert(netif_initialized && !mutex.held && !s_state_lock.entered);
    stops++;
    sntp_enabled = false;
}
void esp_sntp_setserver(uint8_t index, const ip_addr_t *address)
{
    assert(!index && netif_initialized && address && !sntp_enabled);
    assert(address->type == IPADDR_TYPE_V4 && !mutex.held && !s_state_lock.entered);
    active_address = *address; /* API copies the binary address, not its pointer. */
    server_sets++;
}
static int clock_test_getaddrinfo(const char *name, const char *service,
    const struct addrinfo *hints, struct addrinfo **answer)
{
    assert(name && !service && hints && answer && !mutex.held && !s_state_lock.entered);
    assert(hints->ai_family == AF_INET && hints->ai_socktype == SOCK_DGRAM);
    assert(!sntp_enabled); /* DNS never runs inside lwIP SNTP. */
    resolves++;
    strcpy(last_resolved_name, name);
    if (resolver_hook) resolver_hook();
    if (fail_resolves) { fail_resolves--; *answer = NULL; return EAI_FAIL; }
    memset(&dns_sockaddr, 0, sizeof(dns_sockaddr));
    dns_sockaddr.sin_family = AF_INET;
    dns_sockaddr.sin_addr.s_addr = htonl(dns_invalid_address ? UINT32_C(0xe0000001) :
        UINT32_C(0xc0000200) + resolves); /* documentation-only addresses */
    memset(&dns_answer, 0, sizeof(dns_answer));
    dns_answer.ai_family = AF_INET;
    dns_answer.ai_socktype = SOCK_DGRAM;
    dns_answer.ai_addrlen = sizeof(dns_sockaddr);
    dns_answer.ai_addr = (struct sockaddr *)&dns_sockaddr;
    *answer = &dns_answer;
    return 0;
}
static void clock_test_freeaddrinfo(struct addrinfo *answer)
{
    assert(answer == &dns_answer);
    frees++;
}
void sntp_set_sync_status(int status)
{
    assert(status == SNTP_SYNC_STATUS_COMPLETED && !s_state_lock.entered);
    completed++;
}
static int clock_test_settimeofday(const struct timeval *value, const struct timezone *zone)
{
    assert(value && !zone && !s_state_lock.entered && !mutex.held);
    set_calls++;
    if (fail_set) return -1;
    wall = *value;
    return 0;
}
static int clock_test_gettimeofday(struct timeval *value, void *zone)
{
    assert(value && !zone);
    get_calls++;
    if (fail_get) return -1;
    *value = wall;
    return 0;
}
static void clock_test_tzset(void)
{
    tzset();
    /* Darwin does not expose newlib's standard-offset global. Derive it from
       the host libc for tested UTC/US-Pacific zones; never fake localtime. */
    const time_t seasons[] = { 1767225600, 1782864000 }; /* Jan/Jul 2026 */
    bool found = false;
    for (unsigned i = 0; i < 2; ++i) {
        struct tm local;
        assert(localtime_r(&seasons[i], &local));
        if (!local.tm_isdst) {
            clock_test_timezone = -local.tm_gmtoff;
            found = true;
            break;
        }
    }
    assert(found);
}
static size_t clock_test_strftime(char *out, size_t size, const char *format,
    const struct tm *value)
{
    if (strstr(format, "%z")) assert(mutex.held);
    return strftime(out, size, format, value);
}
static void initialize(void)
{
    assert(clock_service_init() == ESP_OK && owner_task);
    assert(!set_calls && !inits && !starts && !deinits);
}
static clock_service_status_t status(void)
{
    clock_service_status_t result;
    assert(clock_service_status_get(&result));
    return result;
}
static void accept_time(int64_t seconds, int64_t micros)
{
    struct timeval value = { (time_t)seconds, (suseconds_t)micros };
    sntp_sync_time(&value);
}
static void run_owner(void (*step)(unsigned))
{
    after_iteration = step;
    if (!setjmp(task_done)) owner_task(NULL);
}
static void reconfigure_step(unsigned step)
{
    if (step == 1) {
        assert(inits == 1 && !deinits && starts == 1 && server_sets == 1);
        strcpy(saved_config.ntp_server, "time.example.test");
        clock_service_config_changed();
        assert(inits == 1 && !deinits && notifications == 1 && starts == 1);
    } else if (step == 2) {
        assert(inits == 1 && !deinits && starts == 2 && server_sets == 2);
        assert(!strcmp(last_resolved_name, "time.example.test"));
        has_ip = false;
        network_revision++;
    } else if (step == 3) {
        assert(inits == 1 && starts == 2 && !sntp_enabled);
        has_ip = true;
        network_revision++;
    } else if (step == 4) {
        assert(starts == 3);
        network_revision++;
    } else if (step == 5) {
        assert(starts == 4 && resolves == 4 && frees == 4 && server_sets == 4);
        assert(status().config_generation == 2 && status().sntp_running);
        longjmp(task_done, 1);
    } else assert(false);
}
static void retry_step(unsigned step)
{
    if (step == 1) {
        assert(inits == 1 && !netif_initialized && status().start_failures == 1);
        uptime_us += 14000000;
    } else if (step == 2) {
        assert(inits == 1);
        uptime_us += 1000000;
    } else if (step == 3) {
        assert(inits == 2 && netif_initialized && status().sntp_running);
        longjmp(task_done, 1);
    } else assert(false);
}
static void offline_step(unsigned step)
{
    assert(inits == 1 && !resolves && !server_sets && !starts && !set_calls && !status().valid);
    if (step == 20) longjmp(task_done, 1);
}
static void dns_change_config(void)
{
    resolver_hook = NULL;
    strcpy(saved_config.ntp_server, "new-time.example.test");
    clock_service_config_changed();
}
static void dns_lose_link(void)
{
    resolver_hook = NULL;
    has_ip = false;
    network_revision++;
}
static void dns_change_network(void)
{
    resolver_hook = NULL;
    network_revision++;
}
static void stale_dns_step(unsigned step)
{
    if (step == 1) {
        assert(resolves == 1 && frees == 1 && !server_sets && !starts);
        assert(!status().sntp_running && !status().start_failures);
    } else if (step == 2) {
        assert(resolves == 2 && frees == 2 && server_sets == 1 && starts == 1);
        assert(active_address.u_addr.ip4.addr == htonl(UINT32_C(0xc0000202)));
        assert(status().sntp_running && !deinits);
        longjmp(task_done, 1);
    } else assert(false);
}
static void link_loss_step(unsigned step)
{
    if (step == 1) {
        assert(resolves == 1 && frees == 1 && !server_sets && !starts);
    } else if (step == 2) {
        assert(resolves == 1 && !starts && !status().sntp_running);
        has_ip = true;
        network_revision++;
    } else if (step == 3) {
        assert(resolves == 2 && frees == 2 && server_sets == 1 && starts == 1);
        longjmp(task_done, 1);
    } else assert(false);
}
static void dns_retry_step(unsigned step)
{
    if (step == 1) {
        assert(resolves == 1 && !server_sets && !starts && status().start_failures == 1);
        assert(!status().sntp_running && status().valid); /* clock holdover survives */
        dns_invalid_address = false;
        uptime_us += 29000000;
    } else if (step == 2) {
        assert(resolves == 1 && !starts);
        uptime_us += 1000000;
    } else if (step == 3) {
        assert(resolves == 2 && starts == 1 && server_sets == 1);
        assert(status().sntp_running && status().valid);
        longjmp(task_done, 1);
    } else assert(false);
}
static void hourly_step(unsigned step)
{
    if (step == 1) {
        assert(resolves == 1 && starts == 1);
        uptime_us += INT64_C(3599000000);
    } else if (step == 2) {
        assert(resolves == 1 && starts == 1);
        uptime_us += 1000000;
    } else if (step == 3) {
        assert(resolves == 2 && frees == 2 && starts == 2 && server_sets == 2 && inits == 1);
        longjmp(task_done, 1);
    } else assert(false);
}
static void retained_test(const char *scenario)
{
    reset_reason = ESP_RST_SW;
    int64_t last = ((int64_t)wall.tv_sec - 3600) * 1000000 + wall.tv_usec;
    bool expected = true;
    if (!strcmp(scenario, "retained-sleep")) reset_reason = ESP_RST_DEEPSLEEP;
    else if (!strcmp(scenario, "retained-power")) { reset_reason = ESP_RST_POWERON; expected = false; }
    else if (!strcmp(scenario, "retained-brownout")) { reset_reason = ESP_RST_BROWNOUT; expected = false; }
    else if (!strcmp(scenario, "retained-panic")) { reset_reason = ESP_RST_PANIC; expected = false; }
    else if (!strcmp(scenario, "retained-stale")) { last -= CLOCK_HOLDOVER_MAX_US; expected = false; }
    else if (!strcmp(scenario, "retained-future")) { last += INT64_C(7200) * 1000000; expected = false; }
    else if (!strcmp(scenario, "retained-bad-time")) { wall.tv_sec = 1; expected = false; }
    else if (!strcmp(scenario, "retained-get-failure")) { fail_get = true; expected = false; }
    else if (!strcmp(scenario, "retained-boundary"))
        last = (int64_t)wall.tv_sec * 1000000 + wall.tv_usec - CLOCK_HOLDOVER_MAX_US;
    s_retained.magic = RETAINED_CLOCK_MAGIC;
    s_retained.last_sync_us = last;
    s_retained.crc = clock_model_retained_checksum(last);
    if (!strcmp(scenario, "retained-crc")) { s_retained.crc ^= 1; expected = false; }
    initialize();
    clock_service_status_t result = status();
    assert(result.valid == expected && !result.synchronized && !set_calls && !completed);
    if (expected) {
        assert(!strcmp(result.source, "retained-ntp") && result.last_sync_unix_us == last);
        assert(result.sync_count == 0);
        assert(s_retained.magic == RETAINED_CLOCK_MAGIC);
    } else assert(!strcmp(result.source, "unsynchronized") && !s_retained.magic);
}
static void timezone_test(void)
{
    strcpy(saved_config.timezone, "PST8PDT,M3.2.0/2,M11.1.0/2");
    initialize();
    const struct { int64_t utc; int hour, month; bool dst; const char *suffix; } cases[] = {
        {1767268800, 4, 0, false, "-0800"}, /* January noon UTC */
        {1782907200, 5, 6, true, "-0700"},  /* July noon UTC */
        {1772963999, 1, 2, false, "-0800"}, /* 2026-03-08 09:59:59 UTC */
        {1772964000, 3, 2, true, "-0700"},
        {1793523599, 1, 10, true, "-0700"}, /* 2026-11-01 08:59:59 UTC */
        {1793523600, 1, 10, false, "-0800"},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        accept_time(cases[i].utc, 0);
        clock_service_snapshot_t snap;
        assert(clock_service_snapshot_get(&snap) && snap.valid);
        assert(snap.utc_offset_minutes == 480 && snap.daylight_saving == cases[i].dst);
        assert(snap.local_time.tm_hour == cases[i].hour && snap.local_time.tm_mon == cases[i].month);
        assert(strstr(status().local_time, cases[i].suffix));
    }
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    if (!strncmp(scenario, "retained-", 9)) retained_test(scenario);
    else if (!strcmp(scenario, "timezone")) timezone_test();
    else {
        initialize();
        if (!strcmp(scenario, "reject-before-set")) {
            struct timeval original = wall;
            sntp_sync_time(NULL);
            accept_time(CLOCK_MIN_UNIX_SECONDS - 1, 0);
            accept_time(CLOCK_MAX_UNIX_SECONDS, 0);
            accept_time(1782864000, -1);
            accept_time(1782864000, 1000000);
            uptime_us = -1; accept_time(1782864000, 0);
            uptime_us = 1; accept_time(CLOCK_MIN_UNIX_SECONDS, 0);
            assert(!set_calls && !completed && status().rejected_syncs == 7);
            assert(!status().valid && !memcmp(&original, &wall, sizeof(wall)));
        } else if (!strcmp(scenario, "set-failure")) {
            fail_set = true; accept_time(wall.tv_sec, wall.tv_usec);
            assert(set_calls == 1 && !completed && !status().valid);
            assert(status().rejected_syncs == 1 && !s_retained.magic);
        } else if (!strcmp(scenario, "accepted-clock")) {
            accept_time(wall.tv_sec, wall.tv_usec);
            assert(set_calls == 1 && completed == 1 && status().sync_count == 1);
            assert(status().valid && status().synchronized && !strcmp(status().source, "ntp"));
            assert(s_retained.magic == RETAINED_CLOCK_MAGIC);
            assert(s_retained.crc == clock_model_retained_checksum(s_retained.last_sync_us));
            uptime_us += 2500000;
            clock_service_snapshot_t snap;
            assert(clock_service_snapshot_get(&snap) && snap.valid);
            assert(snap.utc_unix_us == (int64_t)wall.tv_sec * 1000000 + wall.tv_usec + 2500000);
            assert(snap.boot_utc_unix_us == (int64_t)wall.tv_sec * 1000000 + wall.tv_usec - 5000000);
            assert(!snap.utc_offset_minutes && !snap.daylight_saving);
        } else if (!strcmp(scenario, "snapshot-timeout")) {
            accept_time(wall.tv_sec, wall.tv_usec);
            clock_service_snapshot_t snap;
            fail_take = true;
            assert(!clock_service_snapshot_get(&snap) && !mutex.held);
            assert(!clock_service_snapshot_get(NULL) && !clock_service_status_get(NULL));
        } else if (!strcmp(scenario, "owner-reconfigure")) {
            has_ip = true; run_owner(reconfigure_step);
        } else if (!strcmp(scenario, "owner-retry")) {
            has_ip = true; fail_inits = 1; run_owner(retry_step);
        } else if (!strcmp(scenario, "dns-config-change")) {
            has_ip = true; resolver_hook = dns_change_config; run_owner(stale_dns_step);
            assert(!strcmp(last_resolved_name, "new-time.example.test"));
        } else if (!strcmp(scenario, "dns-link-loss")) {
            has_ip = true; resolver_hook = dns_lose_link; run_owner(link_loss_step);
        } else if (!strcmp(scenario, "dns-network-change")) {
            has_ip = true; resolver_hook = dns_change_network; run_owner(stale_dns_step);
        } else if (!strcmp(scenario, "dns-failure") || !strcmp(scenario, "dns-invalid-address")) {
            has_ip = true;
            accept_time(wall.tv_sec, wall.tv_usec);
            if (!strcmp(scenario, "dns-failure")) fail_resolves = 1;
            else dns_invalid_address = true;
            run_owner(dns_retry_step);
        } else if (!strcmp(scenario, "dns-hourly-refresh")) {
            has_ip = true; run_owner(hourly_step);
        } else if (!strcmp(scenario, "offline-startup")) run_owner(offline_step);
        else assert(false);
    }
    printf("PASS clock service %s\n", scenario);
    return 0;
}
