/* SPDX-License-Identifier: 0BSD */
#include "usb_setup.h"
#include "usb_setup_model.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#include "auth.h"
#include "board_io.h"
#include "config_store.h"
#include "ethernet_manager.h"
#include "firmware.h"

#define SETUP_BOOT_GPIO GPIO_NUM_0
#define SETUP_REPLY_SIZE 1536U

static const char *TAG = "usb_setup";
static usb_setup_model_t s_model;
static int s_usb_fd = -1;

static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void respond(const usb_setup_request_t *request)
{
    cJSON *response = cJSON_CreateObject();
    if (!response) {
        return;
    }
    cJSON_AddStringToObject(response, "id", request->id);
    cJSON_AddNumberToObject(response, "protocol", 1);
    bool allowed = usb_setup_model_key_allowed(&s_model, now_ms());
    if (request->command == USB_SETUP_KEY) {
        uint8_t key[FW_AUTH_KEY_BYTES] = {0};
        bool created = false;
        if (!allowed) {
            cJSON_AddBoolToObject(response, "ok", false);
            cJSON_AddStringToObject(response, "error", "key_locked");
        } else if (config_store_admin_key_get(key, &created) != ESP_OK) {
            cJSON_AddBoolToObject(response, "ok", false);
            cJSON_AddStringToObject(response, "error", "key_unavailable");
        } else {
            char key_hex[FW_AUTH_KEY_BYTES * 2U + 1U];
            auth_hex_encode(key, sizeof(key), key_hex);
            cJSON_AddBoolToObject(response, "ok", true);
            cJSON_AddStringToObject(response, "admin_key", key_hex);
            mbedtls_platform_zeroize(key_hex, sizeof(key_hex));
        }
        mbedtls_platform_zeroize(key, sizeof(key));
    } else if (request->command == USB_SETUP_LOCK) {
        usb_setup_model_lock(&s_model);
        cJSON_AddBoolToObject(response, "ok", true);
    } else {
        firmware_config_t config;
        config_store_get(&config);
        uint8_t mac[6];
        ethernet_manager_mac_get(mac);
        char mac_text[18];
        snprintf(mac_text, sizeof(mac_text), "%02x:%02x:%02x:%02x:%02x:%02x",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        esp_netif_ip_info_t ip_info = {0};
        char address[16] = "";
        if (ethernet_manager_has_ip() && ethernet_manager_get_ip_info(&ip_info)) {
            snprintf(address, sizeof(address), IPSTR, IP2STR(&ip_info.ip));
        }
        cJSON_AddBoolToObject(response, "ok", true);
        cJSON_AddStringToObject(response, "product", FW_PRODUCT_NAME);
        cJSON_AddStringToObject(response, "project", esp_app_get_description()->project_name);
        cJSON_AddStringToObject(response, "firmware_version", esp_app_get_description()->version);
        cJSON_AddStringToObject(response, "ethernet_mac", mac_text);
        cJSON_AddStringToObject(response, "ip_address", address);
        cJSON_AddStringToObject(response, "hostname", config.hostname);
        cJSON_AddNumberToObject(response, "device_instance", config.device_instance);
        cJSON_AddBoolToObject(response, "ethernet_link", ethernet_manager_link_up());
        cJSON_AddBoolToObject(response, "key_export_allowed", allowed);
        cJSON_AddBoolToObject(response, "relay_controller_healthy", board_io_relay_controller_healthy());
        cJSON_AddBoolToObject(response, "rtc_present", board_io_rtc_present());
        cJSON_AddNumberToObject(response, "relay_commands_mask", board_io_relay_commands_mask());
        cJSON_AddNumberToObject(response, "digital_inputs_mask", board_io_inputs_mask());
    }
    /* One bounded VFS write shares the USB console's write lock with logging.
       Key responses go only to native USB, never to UART or application logs. */
    char json[SETUP_REPLY_SIZE];
    char frame[SETUP_REPLY_SIZE + sizeof(USB_SETUP_PREFIX) + 4U];
    if (cJSON_PrintPreallocated(response, json, sizeof(json), false)) {
        int length = snprintf(frame, sizeof(frame), "\n" USB_SETUP_PREFIX " %s\n", json);
        if (length > 0 && (size_t)length < sizeof(frame)) {
            (void)write(s_usb_fd, frame, (size_t)length);
        }
    }
    cJSON *key_item = cJSON_GetObjectItemCaseSensitive(response, "admin_key");
    if (cJSON_IsString(key_item)) {
        mbedtls_platform_zeroize(key_item->valuestring, strlen(key_item->valuestring));
    }
    mbedtls_platform_zeroize(frame, sizeof(frame));
    mbedtls_platform_zeroize(json, sizeof(json));
    cJSON_Delete(response);
}

static void setup_task(void *argument)
{
    (void)argument;
    uint8_t bytes[64];
    for (;;) {
        usb_setup_model_button(&s_model, now_ms(), gpio_get_level(SETUP_BOOT_GPIO) == 0);
        ssize_t length = read(s_usb_fd, bytes, sizeof(bytes));
        for (ssize_t i = 0; i < length; ++i) {
            usb_setup_request_t request;
            if (usb_setup_model_receive(&s_model, bytes[i], &request)) {
                respond(&request);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t usb_setup_start(bool new_admin_key)
{
    gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << SETUP_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t result = gpio_config(&boot_config);
    if (result != ESP_OK) {
        return result;
    }
    result = usb_serial_jtag_vfs_register();
    if (result != ESP_OK) {
        return result;
    }
    const usb_serial_jtag_driver_config_t usb_config = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 512,
    };
    result = usb_serial_jtag_driver_install(&usb_config);
    if (result != ESP_OK) {
        return result;
    }
    /* IDF 5.5 VFS nonblocking reads consult the driver's RX ring buffer.
       Share its buffered TX path with the secondary USB console as well. */
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    s_usb_fd = open("/dev/usbserjtag", O_RDWR | O_NONBLOCK);
    if (s_usb_fd < 0) {
        return ESP_FAIL;
    }
    usb_setup_model_init(&s_model, now_ms(), new_admin_key);
    if (xTaskCreate(setup_task, "usb_setup", 6144, NULL, 3, NULL) != pdPASS) {
        close(s_usb_fd);
        s_usb_fd = -1;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "USB setup ready; hold BOOT for 3 seconds to allow local key download");
    return ESP_OK;
}
