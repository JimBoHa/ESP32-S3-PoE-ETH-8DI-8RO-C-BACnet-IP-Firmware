idf_component_register(
    SRCS
        "app_main.c"
        "auth.c"
        "bacnet_app.c"
        "bacnet_command_trace.c"
        "bacnet_cov_recovery.c"
        "bacnet_instance.c"
        "bip_esp32.c"
        "board_io.c"
        "config_model.c"
        "config_store.c"
        "ethernet_manager.c"
        "usb_setup.c"
        "usb_setup_model.c"
        "web_admin.c"
    INCLUDE_DIRS "."
    REQUIRES
        app_update
        bacnet_stack
        esp_app_format
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_spi
        esp_driver_usb_serial_jtag
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
    EMBED_FILES
        "web/index.html"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wall -Wextra -Werror=return-type -Werror=format
)
