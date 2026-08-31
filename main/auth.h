/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "firmware.h"

esp_err_t auth_init(const uint8_t key[FW_AUTH_KEY_BYTES]);
esp_err_t auth_challenge_issue(char nonce_hex[FW_AUTH_HEX_NONCE_LEN + 1]);
bool auth_request_verify(const char *method, const char *path, const char *nonce_hex,
    size_t content_length, const char *body_sha256_hex, const char *signature_hex,
    char *reason, size_t reason_size);

bool auth_sha256_hex(const uint8_t *data, size_t length,
    char output[FW_AUTH_HEX_SHA256_LEN + 1]);
void auth_hex_encode(const uint8_t *data, size_t length, char *output);
