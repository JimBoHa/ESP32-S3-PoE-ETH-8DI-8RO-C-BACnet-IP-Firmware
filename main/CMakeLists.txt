idf_component_register(
    SRCS
        "app_main.c"
        "auth.c"
        "bacnet_app.c"
        "bip_esp32.c"
        "board_io.c"
        "config_model.c"
        "config_store.c"
        "ethernet_manager.c"
        "web_admin.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bacnet_stack
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_eth
        esp_event
        esp_http_server
        esp_hw_support
        esp_netif
        esp_timer
        json
        lwip
        mbedtls
        nvs_flash
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror=return-type -Werror=format
)
