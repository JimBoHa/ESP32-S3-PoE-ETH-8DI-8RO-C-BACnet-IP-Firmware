/* SPDX-License-Identifier: 0BSD */
#pragma once
#include <stdint.h>
typedef struct { uint32_t addr; } ip4_addr_t;
typedef struct { union { ip4_addr_t ip4; } u_addr; uint8_t type; } ip_addr_t;
#define IPADDR_TYPE_V4 0
#define IP_SET_TYPE_VAL(ipaddr, iptype) ((ipaddr).type = (iptype))
#define ip_2_ip4(ipaddr) (&(ipaddr)->u_addr.ip4)
#define ip4_addr_set_u32(ipaddr, value) ((ipaddr)->addr = (value))
#define ip_addr_set_ip4_u32(ipaddr, value) do { \
    (ipaddr)->u_addr.ip4.addr = (value); (ipaddr)->type = IPADDR_TYPE_V4; \
} while (0)
