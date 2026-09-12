/* SPDX-License-Identifier: 0BSD */
/* Compile real board_io.c separately. All devices and scheduler time are fake. */
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "board_io.h"
#include "config_store.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

enum { REG_INPUT=0, REG_OUTPUT=1, REG_POLARITY=2, REG_CONFIG=3 };
struct board_test_semaphore { bool held; };
static struct board_test_semaphore test_mutex;
static TaskFunction_t health_task;
static void *health_context;
static jmp_buf task_finished;
static unsigned steps_remaining;
static int64_t now_us;
static uint8_t registers[4] = {0, 0xff, 0, 0xff};
static uint8_t gpio_levels;
static unsigned fail_takes, fail_writes, fail_reads, fail_config_writes, fail_polarity_writes;
static unsigned discard_output_writes;
static unsigned total_reads, writes, critical_depth;
static uint8_t write_regs[1024], write_values[1024], driven_masks[1024];
static bool latch_verified_before_write[1024], observed_output_read;
static uint8_t latest_output_read;
static bool baseline;
static bool require_healthy_during_io;
static void (*delay_observer)(void);

#ifdef BOARD_IO_EXPECT_FIXED
static board_io_relay_diagnostics_t diagnostics(void)
{
    board_io_relay_diagnostics_t value;
    assert(!board_io_relay_diagnostics_get(NULL));
    assert(board_io_relay_diagnostics_get(&value));
    assert(value.desired_mask == board_io_relay_commands_mask());
    assert(value.applied_mask == board_io_relays_mask());
    assert(value.healthy == board_io_relay_controller_healthy());
    return value;
}
#endif

static void observe_io(void)
{
    if (!require_healthy_during_io) return;
    /* Interleave a public status read during an unchanged, healthy refresh. */
    assert(board_io_relay_controller_healthy());
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().registers_valid);
#endif
}

static uint8_t driven_mask(void)
{
    /* Model only high output pins. An input/high-Z pin does not drive a relay. */
    return registers[REG_OUTPUT] & (uint8_t)~registers[REG_CONFIG];
}

void board_test_enter_critical(portMUX_TYPE *lock)
{
    assert(lock && lock->entered == 0);
    lock->entered++;
    critical_depth++;
}

void board_test_exit_critical(portMUX_TYPE *lock)
{
    assert(lock && lock->entered == 1 && critical_depth > 0);
    lock->entered--;
    critical_depth--;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &test_mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait)
{
    (void)wait;
    assert(mutex == &test_mutex && critical_depth == 0);
    if (fail_takes) { fail_takes--; return pdFALSE; }
    if (mutex->held) return pdFALSE;
    mutex->held = true;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex)
{
    assert(mutex == &test_mutex && mutex->held);
    mutex->held = false;
    return pdTRUE;
}
BaseType_t xTaskCreate(TaskFunction_t function, const char *name,
    unsigned stack_depth, void *context, BaseType_t priority, TaskHandle_t *handle)
{
    (void)name; (void)stack_depth; (void)priority; (void)handle;
    assert(!health_task);
    health_task = function;
    health_context = context;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks)
{
    assert(ticks == 20 && steps_remaining > 0 && !test_mutex.held && critical_depth == 0);
    if (delay_observer) delay_observer();
    now_us += (int64_t)ticks * 1000;
    if (--steps_remaining == 0) longjmp(task_finished, 1);
}
int64_t esp_timer_get_time(void) { return now_us; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
uint8_t config_store_relay_state_get(void) { return 0xff; }
esp_err_t config_store_relay_state_set(uint8_t state)
{
    (void)state;
    assert(!"persistence disabled in these tests");
    return ESP_FAIL;
}
esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config && config->mode == GPIO_MODE_INPUT);
    return ESP_OK;
}
int gpio_get_level(gpio_num_t gpio)
{
    assert(gpio >= 4 && gpio <= 11);
    return (gpio_levels >> (gpio - 4)) & 1;
}
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *config,
    i2c_master_bus_handle_t *handle)
{
    assert(config && handle);
    *handle = &registers[0];
    return ESP_OK;
}
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *config, i2c_master_dev_handle_t *handle)
{
    assert(bus && config->device_address == 0x20 && handle);
    *handle = &registers[1];
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device,
    const uint8_t *bytes, size_t size, int timeout_ms)
{
    assert(device && bytes && size == 2 && bytes[0] >= 1 && bytes[0] <= 3);
    assert(timeout_ms == 100 && critical_depth == 0);
    observe_io();
    if (fail_writes) { fail_writes--; return ESP_FAIL; }
    if (bytes[0] == REG_CONFIG && fail_config_writes) {
        fail_config_writes--; return ESP_FAIL;
    }
    if (bytes[0] == REG_POLARITY && fail_polarity_writes) {
        fail_polarity_writes--; return ESP_FAIL;
    }
    if (bytes[0] == REG_OUTPUT && discard_output_writes) {
        discard_output_writes--;
    } else {
        registers[bytes[0]] = bytes[1];
    }
    assert(writes < sizeof(write_regs));
    write_regs[writes] = bytes[0];
    write_values[writes] = bytes[1];
    driven_masks[writes] = driven_mask();
    latch_verified_before_write[writes] = observed_output_read &&
        latest_output_read == registers[REG_OUTPUT];
    if (bytes[0] == REG_OUTPUT) observed_output_read = false;
    writes++;
    return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device,
    const uint8_t *request, size_t request_size, uint8_t *response,
    size_t response_size, int timeout_ms)
{
    assert(device && request && request_size == 1 && response && response_size == 1);
    assert(request[0] < 4 && timeout_ms == 100 && critical_depth == 0);
    observe_io();
    total_reads++;
    if (fail_reads) { fail_reads--; return ESP_FAIL; }
    *response = request[0] == REG_INPUT ? driven_mask() : registers[request[0]];
    if (request[0] == REG_OUTPUT) {
        observed_output_read = true;
        latest_output_read = *response;
    }
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t bus, uint16_t address, int timeout_ms)
{
    assert(bus && address == 0x51 && timeout_ms == 100 && critical_depth == 0);
    return ESP_OK;
}

static void init(void)
{
    firmware_config_t config = {0};
    assert(board_io_init(&config) == ESP_OK);
    assert(health_task && board_io_relay_commands_mask() == 0);
    assert(board_io_relays_mask() == 0 && driven_mask() == 0);
    assert(board_io_relay_controller_healthy());
}

static void health_steps(unsigned steps)
{
    /* One bounded task run per scenario; no scheduler wall-clock or sockets. */
    assert(steps > 0);
    steps_remaining = steps;
    if (setjmp(task_finished) == 0) health_task(health_context);
    assert(steps_remaining == 0 && !test_mutex.held && critical_depth == 0);
}

static void reset_expander(void)
{
    registers[REG_OUTPUT] = 0xff;
    registers[REG_POLARITY] = 0;
    registers[REG_CONFIG] = 0xff;
    assert(driven_mask() == 0);
}

static void startup_off(void)
{
    init();
    assert(writes >= 3);
    assert(registers[REG_CONFIG] == 0 && registers[REG_POLARITY] == 0);
    bool latch_cleared = false;
    for (unsigned i = 0; i < writes; i++) {
        assert(driven_masks[i] == 0);
        if (write_regs[i] == REG_OUTPUT && write_values[i] == 0) latch_cleared = true;
        if (write_regs[i] == REG_CONFIG && write_values[i] == 0) {
            assert(latch_cleared);
            if (!baseline) assert(latch_verified_before_write[i]);
        }
    }
    assert(board_io_relay_set(8, true) == ESP_ERR_INVALID_ARG);
    assert(board_io_relay_commands_mask() == 0);
}

static void startup_discarded_latch(void)
{
    firmware_config_t config = {0};
    discard_output_writes = 1;
    esp_err_t result = board_io_init(&config);
    assert((result == ESP_OK) == baseline);
    bool exposed_reset_latch = false;
    for (unsigned i = 0; i < writes; i++) {
        if (driven_masks[i] != 0) exposed_reset_latch = true;
    }
    assert(exposed_reset_latch == baseline);
    if (baseline) {
        puts("  REPRODUCED: startup enables outputs after ACK without verifying cleared latch");
    } else {
        assert(!health_task && registers[REG_CONFIG] == 0xff && driven_mask() == 0);
        assert(registers[REG_OUTPUT] == 0xff);
        assert(!board_io_relay_controller_healthy());
#ifdef BOARD_IO_EXPECT_FIXED
        assert(diagnostics().registers_valid && diagnostics().verification_failures == 1);
#endif
    }
}

static void warm_start_polarity_failure(void)
{
    firmware_config_t config = {0};
    registers[REG_OUTPUT] = 5;
    registers[REG_CONFIG] = 0;
    assert(driven_mask() == 5);
    fail_polarity_writes = 1;
    assert(board_io_init(&config) != ESP_OK);
    assert(fail_polarity_writes == 0 && !health_task);
    assert(registers[REG_CONFIG] == 0 && registers[REG_OUTPUT] == 0);
    assert(driven_mask() == 0 && writes > 0);
    assert(write_regs[0] == REG_OUTPUT && write_values[0] == 0);
    for (unsigned i = 0; i < writes; i++) assert(driven_masks[i] == 0);
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().i2c_errors == 1 && !diagnostics().healthy);
    assert(!diagnostics().registers_valid);
#endif
}

static void normal(void)
{
    init();
    for (unsigned i = 0; i < 8; i++) {
        assert(board_io_relay_set(i, true) == ESP_OK);
        assert(board_io_relay_get(i));
    }
    assert(driven_mask() == 0xff && board_io_relay_commands_mask() == 0xff);
    for (unsigned i = 0; i < 8; i++) assert(board_io_relay_set(i, false) == ESP_OK);
    assert(driven_mask() == 0 && board_io_relays_mask() == 0);
    gpio_levels = 5;
    require_healthy_during_io = true;
    health_steps(3);
    assert(board_io_inputs_mask() == 5 && board_io_rtc_present());
}

static void mutex_timeout(void)
{
    init();
    fail_takes = 1;
    assert(board_io_relay_set(0, true) == ESP_ERR_TIMEOUT);
    assert(board_io_relay_commands_mask() == (baseline ? 0 : 1));
    assert(driven_mask() == 0);
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().mutex_timeouts == 1 && !diagnostics().registers_valid);
#endif
    health_steps(51);
    assert(driven_mask() == (baseline ? 0 : 1));
    assert(board_io_relays_mask() == (baseline ? 0 : 1));
    if (baseline) puts("  REPRODUCED: mutex timeout discards desired On; health retries old Off");
}

static void mutex_off_recovery(void)
{
    init();
    assert(board_io_relay_set(0, true) == ESP_OK);
    fail_takes = 1;
    assert(board_io_relay_set(0, false) == ESP_ERR_TIMEOUT);
    assert(board_io_relay_commands_mask() == (baseline ? 1 : 0));
    assert(driven_mask() == 1);
    assert(board_io_relay_controller_healthy() == baseline);
    unsigned start_write = writes;
    health_steps(51);
    assert(driven_mask() == (baseline ? 1 : 0));
    assert(board_io_relays_mask() == (baseline ? 1 : 0));
    for (unsigned i = start_write; i < writes; i++) {
        assert(driven_masks[i] == (baseline ? 1 : 0));
    }
}

static void mutex_latest_wins(void)
{
    init();
    fail_takes = 3;
    assert(board_io_relay_set(0, true) == ESP_ERR_TIMEOUT);
    assert(board_io_relay_set(1, true) == ESP_ERR_TIMEOUT);
    assert(board_io_relay_set(0, false) == ESP_ERR_TIMEOUT);
    assert(board_io_relay_commands_mask() == (baseline ? 0 : 2));
    health_steps(51);
    assert(driven_mask() == (baseline ? 0 : 2));
}

static void i2c_retry(void)
{
    init();
    fail_writes = 1;
    assert(board_io_relay_set(0, true) != ESP_OK);
    assert(board_io_relay_commands_mask() == 1 && driven_mask() == 0);
    assert(!board_io_relay_controller_healthy());
    health_steps(51);
    assert(driven_mask() == 1 && board_io_relays_mask() == 1);
    assert(board_io_relay_controller_healthy());
}

static void expander_reset(void)
{
    init();
    assert(board_io_relay_set(0, true) == ESP_OK);
    assert(board_io_relay_set(2, true) == ESP_OK);
    reset_expander();
    unsigned start_write = writes;
    health_steps(51);
    assert(board_io_relay_commands_mask() == 5);
    assert(board_io_relay_controller_healthy());
    assert(driven_mask() == (baseline ? 0 : 5));
    assert(registers[REG_CONFIG] == (baseline ? 0xff : 0));
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().configuration_recoveries == 1);
    assert(diagnostics().registers_valid && diagnostics().configuration_register == 0);
    assert(diagnostics().output_register == 5 && diagnostics().last_error == ESP_OK);
#endif
    for (unsigned i = start_write; i < writes; i++) {
        assert((driven_masks[i] & (uint8_t)~5U) == 0);
        if (write_regs[i] == REG_CONFIG && write_values[i] == 0) {
            assert(i > start_write);
            bool prepared = false;
            for (unsigned j = start_write; j < i; j++) {
                if (write_regs[j] == REG_OUTPUT && write_values[j] == 5) prepared = true;
            }
            assert(prepared);
        }
    }
    if (baseline) puts("  REPRODUCED: latch says On and healthy; CONFIG=0xff leaves outputs high-Z");
}

static void read_failure(void)
{
    init();
    fail_reads = 1;
    unsigned reads_before = total_reads;
    health_steps(1);
    assert(board_io_relay_controller_healthy() == baseline);
    assert(baseline ? total_reads == reads_before : total_reads > reads_before);
#ifdef BOARD_IO_EXPECT_FIXED
    assert(!diagnostics().registers_valid && diagnostics().i2c_errors == 1);
#endif
}

static void observe_failed_config_recovery(void)
{
    if (now_us != 0) return;
    assert(board_io_relay_controller_healthy() == baseline);
    assert(driven_mask() == 0 && registers[REG_CONFIG] == 0xff);
    assert(board_io_relay_commands_mask() == 1);
    assert(baseline ? fail_config_writes == 1 : fail_config_writes == 0);
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().i2c_errors == 1 && !diagnostics().registers_valid);
    assert(diagnostics().configuration_recoveries == 0);
#endif
}

static void config_write_failure(void)
{
    init();
    assert(board_io_relay_set(0, true) == ESP_OK);
    reset_expander();
    fail_config_writes = 1;
    delay_observer = observe_failed_config_recovery;
    health_steps(51);
    assert(board_io_relay_controller_healthy());
    assert(driven_mask() == (baseline ? 0 : 1));
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().configuration_recoveries == 1 && diagnostics().registers_valid);
#endif
}

static void latch_mismatch(void)
{
    init();
    discard_output_writes = 1;
    esp_err_t result = board_io_relay_set(0, true);
    assert((result == ESP_OK) == baseline);
    assert(board_io_relay_commands_mask() == 1 && driven_mask() == 0);
    assert(board_io_relay_controller_healthy() == baseline);
    assert(board_io_relays_mask() == (baseline ? 1 : 0));
#ifdef BOARD_IO_EXPECT_FIXED
    assert(diagnostics().registers_valid && diagnostics().verification_failures == 1);
    assert(diagnostics().output_register == 0 && diagnostics().configuration_register == 0);
#endif
    health_steps(51);
    assert(driven_mask() == 1 && board_io_relay_controller_healthy());
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    baseline = strcmp(argv[2], "baseline") == 0;
    assert(baseline || strcmp(argv[2], "fixed") == 0);
    if (!strcmp(argv[1], "startup-off")) startup_off();
    else if (!strcmp(argv[1], "startup-discarded-latch")) startup_discarded_latch();
    else if (!strcmp(argv[1], "warm-start-polarity-failure")) warm_start_polarity_failure();
    else if (!strcmp(argv[1], "normal")) normal();
    else if (!strcmp(argv[1], "mutex-timeout")) mutex_timeout();
    else if (!strcmp(argv[1], "mutex-latest-wins")) mutex_latest_wins();
    else if (!strcmp(argv[1], "mutex-off-recovery")) mutex_off_recovery();
    else if (!strcmp(argv[1], "i2c-retry")) i2c_retry();
    else if (!strcmp(argv[1], "expander-reset")) expander_reset();
    else if (!strcmp(argv[1], "read-failure")) read_failure();
    else if (!strcmp(argv[1], "config-write-failure")) config_write_failure();
    else if (!strcmp(argv[1], "latch-mismatch")) latch_mismatch();
    else assert(!"unknown scenario");
    printf("PASS board_io %s (%s)\n", argv[1], argv[2]);
    return 0;
}
