/* SPDX-License-Identifier: 0BSD */
#include "board_io.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "config_store.h"
#include "firmware.h"

#define BOARD_I2C_PORT I2C_NUM_0
#define BOARD_I2C_SDA_GPIO GPIO_NUM_42
#define BOARD_I2C_SCL_GPIO GPIO_NUM_41
#define BOARD_I2C_HZ 100000U
#define TCA9554_ADDRESS 0x20U
#define PCF85063_ADDRESS 0x51U
#define TCA9554_REG_OUTPUT 0x01U
#define TCA9554_REG_POLARITY 0x02U
#define TCA9554_REG_CONFIG 0x03U
#define I2C_TIMEOUT_MS 100
#define INPUT_POLL_MS 20U
#define INPUT_DEBOUNCE_SAMPLES 3U
#define RELAY_SAVE_DELAY_US (5LL * 1000LL * 1000LL)

static const char *TAG = "board_io";
static const gpio_num_t INPUT_GPIOS[FW_DI_COUNT] = {
    GPIO_NUM_4, GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11,
};

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_tca9554;
static SemaphoreHandle_t s_relay_mutex;
/* Never hold this spinlock across I2C or an RTOS wait. Publishing a new
   effective command must not depend on acquiring the slow hardware mutex. */
static portMUX_TYPE s_relay_state_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t s_input_mask;
static volatile uint8_t s_relay_mask;
static volatile uint8_t s_relay_desired_mask;
static volatile bool s_tca_healthy;
static volatile bool s_rtc_present;
static uint8_t s_invert_mask;
static bool s_restore_relay_state;
static bool s_relay_save_dirty;
static int64_t s_relay_last_change_us;
static uint32_t s_relay_change_revision;
static board_io_relay_diagnostics_t s_relay_diagnostics;

static uint8_t relay_desired_mask_get(void)
{
    portENTER_CRITICAL(&s_relay_state_lock);
    uint8_t mask = s_relay_desired_mask;
    portEXIT_CRITICAL(&s_relay_state_lock);
    return mask;
}

static esp_err_t relay_io_result(esp_err_t result)
{
    if (result != ESP_OK) {
        portENTER_CRITICAL(&s_relay_state_lock);
        s_tca_healthy = false;
        s_relay_diagnostics.registers_valid = false;
        s_relay_diagnostics.i2c_errors++;
        s_relay_diagnostics.last_error = result;
        portEXIT_CRITICAL(&s_relay_state_lock);
    }
    return result;
}

static esp_err_t tca9554_write_register(uint8_t reg, uint8_t value)
{
    uint8_t bytes[2] = {reg, value};
    esp_err_t result = i2c_master_transmit(s_tca9554, bytes, sizeof(bytes), I2C_TIMEOUT_MS);
    return relay_io_result(result);
}

static esp_err_t tca9554_read_register(uint8_t reg, uint8_t *value)
{
    return relay_io_result(i2c_master_transmit_receive(s_tca9554,
        &reg, sizeof(reg), value, sizeof(*value), I2C_TIMEOUT_MS));
}

static esp_err_t relay_registers_read_locked(uint8_t *output, uint8_t *configuration)
{
    esp_err_t result = tca9554_read_register(TCA9554_REG_CONFIG, configuration);
    if (result != ESP_OK) {
        return result;
    }
    result = tca9554_read_register(TCA9554_REG_OUTPUT, output);
    if (result == ESP_OK) {
        portENTER_CRITICAL(&s_relay_state_lock);
        s_relay_diagnostics.output_register = *output;
        s_relay_diagnostics.configuration_register = *configuration;
        s_relay_diagnostics.registers_valid = true;
        portEXIT_CRITICAL(&s_relay_state_lock);
    }
    return result;
}

static esp_err_t relay_verification_failed(void)
{
    portENTER_CRITICAL(&s_relay_state_lock);
    s_tca_healthy = false;
    s_relay_diagnostics.verification_failures++;
    s_relay_diagnostics.last_error = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_relay_state_lock);
    return ESP_ERR_INVALID_STATE;
}

/* Caller holds s_relay_mutex (or is initializing before the task exists).
   No unbounded retry: each failure leaves the latest desired command pending
   for the next periodic health pass. The TCA has no register auto-increment. */
static esp_err_t relay_mask_write_locked(uint8_t mask)
{
    esp_err_t result = tca9554_write_register(TCA9554_REG_OUTPUT, mask);
    if (result != ESP_OK) {
        return result;
    }
    uint8_t output = 0, configuration = 0;
    result = relay_registers_read_locked(&output, &configuration);
    if (result != ESP_OK) {
        return result;
    }
    if (output != mask) {
        /* Never enable outputs with an unverified latch: a reset latch is FF. */
        return relay_verification_failed();
    }

    bool recovered = configuration != 0;
    if (recovered) {
        portENTER_CRITICAL(&s_relay_state_lock);
        s_tca_healthy = false;
        portEXIT_CRITICAL(&s_relay_state_lock);
        /* A TCA-only reset returns its pins to inputs. Successful latch writes
           cannot repair that. Load/verify the desired latch BEFORE enabling
           the pins; do not force every relay off or revive a saved command. */
        result = tca9554_write_register(TCA9554_REG_POLARITY, 0);
        if (result != ESP_OK) {
            return result;
        }
        result = tca9554_write_register(TCA9554_REG_CONFIG, 0);
        if (result != ESP_OK) {
            return result;
        }
        result = relay_registers_read_locked(&output, &configuration);
        if (result != ESP_OK) {
            return result;
        }
    }
    if (configuration != 0 || output != mask) {
        return relay_verification_failed();
    }

    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_relay_state_lock);
    uint8_t old_mask = s_relay_mask;
    s_relay_mask = mask;
    s_tca_healthy = true;
    s_relay_diagnostics.last_error = ESP_OK;
    s_relay_diagnostics.last_verified_ms = (uint64_t)now_us / 1000U;
    if (recovered) {
        s_relay_diagnostics.configuration_recoveries++;
    }
    portEXIT_CRITICAL(&s_relay_state_lock);
    if (old_mask != mask && s_restore_relay_state) {
        s_relay_last_change_us = now_us;
        s_relay_save_dirty = true;
        s_relay_change_revision++;
    }
    return ESP_OK;
}

static void input_and_health_task(void *context)
{
    (void)context;
    uint8_t candidate_mask = 0;
    uint8_t stable_counts[FW_DI_COUNT] = {0};
    int64_t next_health_check_us = 0;

    for (;;) {
        uint8_t sample_mask = 0;
        for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
            bool raw_high = gpio_get_level(INPUT_GPIOS[i]) != 0;
            bool active = ((s_invert_mask >> i) & 1U) ? !raw_high : raw_high;
            if (active) {
                sample_mask |= (uint8_t)(1U << i);
            }

            bool sample = (sample_mask & (1U << i)) != 0;
            bool candidate = (candidate_mask & (1U << i)) != 0;
            if (sample == candidate) {
                if (stable_counts[i] < INPUT_DEBOUNCE_SAMPLES) {
                    stable_counts[i]++;
                }
            } else {
                candidate_mask ^= (uint8_t)(1U << i);
                stable_counts[i] = 1;
            }
            if (stable_counts[i] >= INPUT_DEBOUNCE_SAMPLES) {
                if (sample) {
                    s_input_mask |= (uint8_t)(1U << i);
                } else {
                    s_input_mask &= (uint8_t)~(1U << i);
                }
            }
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us >= next_health_check_us) {
            next_health_check_us = now_us + 1000000LL;
            if (xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                /* Retry the BACnet-effective command after a transient I2C
                   failure instead of silently preserving a stale output. */
                (void)relay_mask_write_locked(relay_desired_mask_get());
                xSemaphoreGive(s_relay_mutex);
            }
            s_rtc_present = i2c_master_probe(s_i2c_bus, PCF85063_ADDRESS, I2C_TIMEOUT_MS) == ESP_OK;
        }

        bool should_save = false;
        uint8_t state = 0;
        uint32_t revision = 0;
        if (s_restore_relay_state &&
            xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (s_relay_save_dirty &&
                now_us - s_relay_last_change_us >= RELAY_SAVE_DELAY_US) {
                should_save = true;
                state = s_relay_mask;
                revision = s_relay_change_revision;
            }
            xSemaphoreGive(s_relay_mutex);
        }
        if (should_save) {
            esp_err_t result = config_store_relay_state_set(state);
            if (xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                if (result == ESP_OK && revision == s_relay_change_revision &&
                    state == s_relay_mask) {
                    s_relay_save_dirty = false;
                } else if (result != ESP_OK) {
                    s_relay_last_change_us = now_us;
                }
                xSemaphoreGive(s_relay_mutex);
            }
            if (result != ESP_OK) {
                ESP_LOGW(TAG, "Could not persist relay state: %s", esp_err_to_name(result));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(INPUT_POLL_MS));
    }
}

esp_err_t board_io_init(const firmware_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }
    s_invert_mask = config->input_invert_mask;
    s_restore_relay_state = config->restore_relay_state;
    s_relay_mutex = xSemaphoreCreateMutex();
    if (!s_relay_mutex) {
        return ESP_ERR_NO_MEM;
    }

    uint64_t gpio_mask = 0;
    for (unsigned i = 0; i < FW_DI_COUNT; ++i) {
        gpio_mask |= 1ULL << INPUT_GPIOS[i];
    }
    gpio_config_t input_config = {
        .pin_bit_mask = gpio_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), TAG, "configure digital inputs");

    i2c_master_bus_config_t bus_config = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "create I2C bus");

    i2c_device_config_t tca_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = TCA9554_ADDRESS,
        .scl_speed_hz = BOARD_I2C_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &tca_config, &s_tca9554),
        TAG, "attach TCA9554");

    /* Apply Off first, including an ESP-only restart with TCA outputs still
       enabled. Verify that latch BEFORE enabling any reset-input pins; an
       ACKed but discarded latch write must never expose the reset FF latch. */
    portENTER_CRITICAL(&s_relay_state_lock);
    s_relay_desired_mask = 0;
    portEXIT_CRITICAL(&s_relay_state_lock);
    ESP_RETURN_ON_ERROR(relay_mask_write_locked(0), TAG, "verify relay outputs off");
    /* The cold-start recovery path already set polarity before enabling pins.
       Also initialize it when the expander kept its output configuration. */
    ESP_RETURN_ON_ERROR(tca9554_write_register(TCA9554_REG_POLARITY, 0), TAG, "set relay polarity");
    /* Enabling the initial reset-input configuration is normal startup, not
       evidence that the expander lost its configuration during operation. */
    portENTER_CRITICAL(&s_relay_state_lock);
    s_relay_diagnostics.configuration_recoveries = 0;
    portEXIT_CRITICAL(&s_relay_state_lock);

    if (s_restore_relay_state) {
        uint8_t restored = config_store_relay_state_get();
        portENTER_CRITICAL(&s_relay_state_lock);
        s_relay_desired_mask = restored;
        portEXIT_CRITICAL(&s_relay_state_lock);
        ESP_RETURN_ON_ERROR(relay_mask_write_locked(restored), TAG, "restore relay state");
        ESP_LOGW(TAG, "Relay restore enabled; restored mask 0x%02x", restored);
    }

    s_rtc_present = i2c_master_probe(s_i2c_bus, PCF85063_ADDRESS, I2C_TIMEOUT_MS) == ESP_OK;
    BaseType_t created = xTaskCreate(input_and_health_task, "board_io", 4096, NULL, 5, NULL);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "8DI/8RO initialized; relays=%02x RTC=%s", s_relay_mask,
        s_rtc_present ? "present" : "not detected");
    return ESP_OK;
}

bool board_io_input_get(unsigned index)
{
    return index < FW_DI_COUNT && (s_input_mask & (1U << index)) != 0;
}

uint8_t board_io_inputs_mask(void)
{
    return s_input_mask;
}

esp_err_t board_io_relay_set(unsigned index, bool active)
{
    if (index >= FW_RELAY_COUNT || !s_relay_mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_relay_state_lock);
    uint8_t new_mask = active ? (uint8_t)(s_relay_desired_mask | (1U << index)) :
        (uint8_t)(s_relay_desired_mask & ~(1U << index));
    s_relay_desired_mask = new_mask;
    portEXIT_CRITICAL(&s_relay_state_lock);
    if (xSemaphoreTake(s_relay_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        portENTER_CRITICAL(&s_relay_state_lock);
        s_tca_healthy = false;
        s_relay_diagnostics.registers_valid = false;
        s_relay_diagnostics.mutex_timeouts++;
        s_relay_diagnostics.last_error = ESP_ERR_TIMEOUT;
        portEXIT_CRITICAL(&s_relay_state_lock);
        return ESP_ERR_TIMEOUT;
    }
    /* Re-read after the wait: a later command must supersede an older one. */
    esp_err_t result = relay_mask_write_locked(relay_desired_mask_get());
    xSemaphoreGive(s_relay_mutex);
    return result;
}

bool board_io_relay_get(unsigned index)
{
    return index < FW_RELAY_COUNT && (board_io_relays_mask() & (1U << index)) != 0;
}

uint8_t board_io_relays_mask(void)
{
    portENTER_CRITICAL(&s_relay_state_lock);
    uint8_t mask = s_relay_mask;
    portEXIT_CRITICAL(&s_relay_state_lock);
    return mask;
}

uint8_t board_io_relay_commands_mask(void)
{
    return relay_desired_mask_get();
}

bool board_io_relay_controller_healthy(void)
{
    portENTER_CRITICAL(&s_relay_state_lock);
    bool healthy = s_tca_healthy && s_relay_desired_mask == s_relay_mask;
    portEXIT_CRITICAL(&s_relay_state_lock);
    return healthy;
}

bool board_io_relay_diagnostics_get(board_io_relay_diagnostics_t *diagnostics)
{
    if (!diagnostics) {
        return false;
    }
    portENTER_CRITICAL(&s_relay_state_lock);
    *diagnostics = s_relay_diagnostics;
    diagnostics->desired_mask = s_relay_desired_mask;
    diagnostics->applied_mask = s_relay_mask;
    diagnostics->healthy = s_tca_healthy && s_relay_desired_mask == s_relay_mask;
    portEXIT_CRITICAL(&s_relay_state_lock);
    return true;
}

bool board_io_rtc_present(void)
{
    return s_rtc_present;
}
