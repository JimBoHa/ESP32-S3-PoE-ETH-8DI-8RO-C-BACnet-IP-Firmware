/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#define FW_PRODUCT_NAME "ESP32-S3-PoE-ETH-8DI-8RO-C BACnet/IP"
#define FW_MODEL_NAME "ESP32-S3-PoE-ETH-8DI-8RO-C"
#define FW_DEFAULT_VENDOR_NAME "BACnet Stack at SourceForge"
#define FW_DEFAULT_VENDOR_ID 260U

#define FW_DI_COUNT 8U
#define FW_RELAY_COUNT 8U
#define FW_STATUS_BI_COUNT 4U
#define FW_STATUS_AI_COUNT 4U

#define FW_DEFAULT_DEVICE_INSTANCE 599153U
#define FW_DEFAULT_BACNET_PORT 47808U

#define FW_STATUS_BI_ETHERNET_NAME "Status Ethernet Link"
#define FW_STATUS_BI_IPV4_NAME "Status IPv4 Assigned"
#define FW_STATUS_BI_RELAY_NAME "Status Relay Controller"
#define FW_STATUS_BI_RTC_NAME "Status RTC Present"
#define FW_STATUS_AI_UPTIME_NAME "Status Uptime Seconds"
#define FW_STATUS_AI_HEAP_NAME "Status Free Heap Bytes"
#define FW_STATUS_AI_MIN_HEAP_NAME "Status Minimum Heap Bytes"
#define FW_STATUS_AI_REBOOTS_NAME "Status Reboot Count"
#define FW_NETWORK_PORT_NAME "BACnet/IP Ethernet Port"

#define FW_HTTP_PORT 80U
#define FW_AUTH_KEY_BYTES 32U
#define FW_AUTH_NONCE_BYTES 16U
#define FW_AUTH_HEX_SHA256_LEN 64U
#define FW_AUTH_HEX_NONCE_LEN 32U
#define FW_AUTH_CHALLENGE_TTL_SECONDS 60U

#define FW_CONFIG_MAGIC 0x42414349UL /* "BACI" */
#define FW_CONFIG_SCHEMA 1U

#define FW_HOSTNAME_LEN 32U
#define FW_NAME_LEN 48U
#define FW_LOCATION_LEN 64U
#define FW_VENDOR_NAME_LEN 48U
#define FW_IPV4_TEXT_LEN 16U
