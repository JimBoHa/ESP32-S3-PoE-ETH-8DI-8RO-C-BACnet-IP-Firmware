/* SPDX-License-Identifier: Apache-2.0 */
#include "ethernet_manager.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"

#define W5500_SPI_HOST SPI2_HOST
#define W5500_MOSI_GPIO GPIO_NUM_13
#define W5500_MISO_GPIO GPIO_NUM_14
#define W5500_SCLK_GPIO GPIO_NUM_15
#define W5500_CS_GPIO GPIO_NUM_16
#define W5500_IRQ_GPIO GPIO_NUM_12
#define W5500_RESET_GPIO GPIO_NUM_39
#define W5500_PHY_ADDRESS 1
#define W5500_SPI_CLOCK_HZ (20 * 1000 * 1000)
#define NETWORK_IP_BIT BIT0

static const char *TAG = "ethernet";
static EventGroupHandle_t s_events;
static esp_eth_handle_t s_eth_handle;
static esp_netif_t *s_netif;
static esp_netif_ip_info_t s_ip_info;
static uint8_t s_mac[6];
static volatile bool s_link_up;
static volatile bool s_has_ip;
static volatile uint32_t s_network_revision;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static void ethernet_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_link_up = true;
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
        s_link_up = false;
        portENTER_CRITICAL(&s_state_lock);
        s_has_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        s_network_revision++;
        portEXIT_CRITICAL(&s_state_lock);
        xEventGroupClearBits(s_events, NETWORK_IP_BIT);
        ESP_LOGW(TAG, "Ethernet link down");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        portENTER_CRITICAL(&s_state_lock);
        s_ip_info = event->ip_info;
        s_has_ip = true;
        s_network_revision++;
        portEXIT_CRITICAL(&s_state_lock);
        xEventGroupSetBits(s_events, NETWORK_IP_BIT);
        ESP_LOGI(TAG, "IPv4 " IPSTR " mask " IPSTR " gateway " IPSTR,
            IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask),
            IP2STR(&event->ip_info.gw));
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        portENTER_CRITICAL(&s_state_lock);
        s_has_ip = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        s_network_revision++;
        portEXIT_CRITICAL(&s_state_lock);
        xEventGroupClearBits(s_events, NETWORK_IP_BIT);
        ESP_LOGW(TAG, "Ethernet lost IPv4 address");
    }
}

static esp_err_t configure_static_ipv4(const firmware_config_t *config)
{
    esp_netif_ip_info_t info = {0};
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(config->ip_address, &info.ip), TAG, "parse static IP");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(config->netmask, &info.netmask), TAG, "parse netmask");
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(config->gateway, &info.gw), TAG, "parse gateway");

    esp_err_t result = esp_netif_dhcpc_stop(s_netif);
    if (result != ESP_OK && result != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return result;
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_netif, &info), TAG, "set static IPv4");

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(config->dns_server, &dns.ip.u_addr.ip4),
        TAG, "parse DNS server");
    if (dns.ip.u_addr.ip4.addr == 0U) {
        return ESP_OK;
    }
    return esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
}

esp_err_t ethernet_manager_init(const firmware_config_t *config)
{
    if (!config) {
        return ESP_ERR_INVALID_ARG;
    }

    /* SPI Ethernet MACs use a GPIO interrupt to signal received frames. The
       W5500 driver adds its per-pin handler during esp_eth_driver_install(),
       so the shared GPIO ISR service must already exist at that point. */
    esp_err_t isr_result = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(isr_result == ESP_OK || isr_result == ESP_ERR_INVALID_STATE,
        isr_result, TAG, "install GPIO ISR service");
    if (isr_result == ESP_OK) {
        ESP_LOGI(TAG, "GPIO ISR service installed");
    }

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = W5500_MOSI_GPIO,
        .miso_io_num = W5500_MISO_GPIO,
        .sclk_io_num = W5500_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 2048,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG, "initialize W5500 SPI bus");

    spi_device_interface_config_t device_config = {
        .mode = 0,
        .clock_speed_hz = W5500_SPI_CLOCK_HZ,
        .spics_io_num = W5500_CS_GPIO,
        .queue_size = 20,
    };
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = W5500_PHY_ADDRESS;
    phy_config.reset_gpio_num = W5500_RESET_GPIO;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &device_config);
    w5500_config.int_gpio_num = W5500_IRQ_GPIO;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG, "create W5500 driver objects");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "install W5500 driver");
    /* W5500 has no factory-programmed MAC. Match Espressif's SPI Ethernet
       convention: derive a stable, locally administered address from the
       ESP32's unique Ethernet base address. */
    uint8_t base_mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(base_mac, ESP_MAC_ETH), TAG, "read Ethernet base MAC");
    esp_derive_local_mac(s_mac, base_mac);
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, s_mac), TAG, "set Ethernet MAC");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_netif, ESP_ERR_NO_MEM, TAG, "create Ethernet netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_netif, config->hostname), TAG, "set hostname");
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(glue, ESP_ERR_NO_MEM, TAG, "create Ethernet netif glue");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, glue), TAG, "attach Ethernet netif");

    if (!config->dhcp_enabled) {
        ESP_RETURN_ON_ERROR(configure_static_ipv4(config), TAG, "configure static IPv4");
    }
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
        ethernet_event_handler, NULL), TAG, "register Ethernet events");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
        ip_event_handler, NULL), TAG, "register got-IP event");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
        ip_event_handler, NULL), TAG, "register lost-IP event");
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "start Ethernet");

    ESP_LOGI(TAG, "W5500 started, MAC %02x:%02x:%02x:%02x:%02x:%02x, IPv4=%s",
        s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5],
        config->dhcp_enabled ? "DHCP" : config->ip_address);
    return ESP_OK;
}

bool ethernet_manager_wait_for_ip(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, NETWORK_IP_BIT, pdFALSE, pdTRUE,
        timeout_ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms));
    return (bits & NETWORK_IP_BIT) != 0;
}

bool ethernet_manager_link_up(void)
{
    return s_link_up;
}

bool ethernet_manager_has_ip(void)
{
    return s_has_ip;
}

uint32_t ethernet_manager_network_revision(void)
{
    return s_network_revision;
}

bool ethernet_manager_get_ip_info(esp_netif_ip_info_t *info)
{
    if (!info) {
        return false;
    }
    portENTER_CRITICAL(&s_state_lock);
    *info = s_ip_info;
    bool valid = s_has_ip;
    portEXIT_CRITICAL(&s_state_lock);
    return valid;
}

void ethernet_manager_mac_get(uint8_t mac[6])
{
    if (mac) {
        memcpy(mac, s_mac, sizeof(s_mac));
    }
}
