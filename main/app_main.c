/* SPDX-License-Identifier: Apache-2.0 */
#include <stdbool.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/platform_util.h"
#include "nvs_flash.h"

#include "auth.h"
#include "bacnet_app.h"
#include "board_io.h"
#include "config_store.h"
#include "ethernet_manager.h"
#include "firmware.h"
#include "web_admin.h"

static const char *TAG = "app";

static bool running_image_pending_verification(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
}

static void fatal_startup_error(esp_err_t error, const char *stage, bool pending_verify)
{
    ESP_LOGE(TAG, "Fatal startup failure at %s: %s", stage, esp_err_to_name(error));
    if (pending_verify) {
        ESP_LOGE(TAG, "Rejecting new OTA image and rolling back");
        ESP_ERROR_CHECK(esp_ota_mark_app_invalid_rollback_and_reboot());
    }
    ESP_ERROR_CHECK(error);
}

void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "%s firmware %s", FW_PRODUCT_NAME, app->version);
    ESP_LOGI(TAG, "ESP-IDF reset reason code: %d", (int)esp_reset_reason());
    bool pending_verify = running_image_pending_verification();

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Reformatting incompatible NVS partition");
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    if (result != ESP_OK) {
        fatal_startup_error(result, "NVS", pending_verify);
    }
    if ((result = config_store_init()) != ESP_OK) {
        fatal_startup_error(result, "configuration", pending_verify);
    }

    uint8_t admin_key[FW_AUTH_KEY_BYTES];
    bool key_created = false;
    if ((result = config_store_admin_key_get(admin_key, &key_created)) != ESP_OK) {
        fatal_startup_error(result, "admin key", pending_verify);
    }
    if ((result = auth_init(admin_key)) != ESP_OK) {
        fatal_startup_error(result, "authentication", pending_verify);
    }
    if (key_created) {
        char key_hex[FW_AUTH_KEY_BYTES * 2U + 1U];
        auth_hex_encode(admin_key, sizeof(admin_key), key_hex);
        ESP_LOGW(TAG, "============================================================");
        ESP_LOGW(TAG, "FIRST-BOOT ADMIN KEY (capture now; it is not shown again):");
        ESP_LOGW(TAG, "%s", key_hex);
        ESP_LOGW(TAG, "Store it in the commissioning machine's protected key file.");
        ESP_LOGW(TAG, "============================================================");
        mbedtls_platform_zeroize(key_hex, sizeof(key_hex));
    }
    mbedtls_platform_zeroize(admin_key, sizeof(admin_key));

    firmware_config_t config;
    config_store_get(&config);
    if (config.vendor_id == FW_DEFAULT_VENDOR_ID &&
        strcmp(config.vendor_name, FW_DEFAULT_VENDOR_NAME) == 0) {
        ESP_LOGW(TAG, "Using bacnet-stack vendor identity %u; configure the owner's "
            "assigned BACnet vendor ID before product distribution", config.vendor_id);
    }
    if ((result = board_io_init(&config)) != ESP_OK) {
        fatal_startup_error(result, "8DI/8RO hardware", pending_verify);
    }
    if ((result = esp_netif_init()) != ESP_OK) {
        fatal_startup_error(result, "TCP/IP", pending_verify);
    }
    if ((result = esp_event_loop_create_default()) != ESP_OK) {
        fatal_startup_error(result, "event loop", pending_verify);
    }
    if ((result = ethernet_manager_init(&config)) != ESP_OK) {
        fatal_startup_error(result, "W5500 Ethernet", pending_verify);
    }

    if ((result = bacnet_app_start(&config)) != ESP_OK) {
        fatal_startup_error(result, "BACnet service", pending_verify);
    }
    if ((result = web_admin_start()) != ESP_OK) {
        fatal_startup_error(result, "management service", pending_verify);
    }

    /* Core self-test passed: NVS, TCA9554, W5500 driver, BACnet task, and
       management server initialized. Link and DHCP are deliberately not
       required because a cable can be absent. */
    if (pending_verify) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "OTA image passed startup self-test and is now confirmed");
    }
}
