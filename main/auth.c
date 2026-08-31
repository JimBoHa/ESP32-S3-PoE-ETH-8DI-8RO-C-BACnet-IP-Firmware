/* SPDX-License-Identifier: Apache-2.0 */
#include "auth.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#define AUTH_CHALLENGE_SLOTS 4U
#define AUTH_CANONICAL_MAX 384U

typedef struct {
    bool valid;
    int64_t issued_us;
    char nonce[FW_AUTH_HEX_NONCE_LEN + 1];
} auth_challenge_t;

static uint8_t s_key[FW_AUTH_KEY_BYTES];
static auth_challenge_t s_challenges[AUTH_CHALLENGE_SLOTS];
static unsigned s_next_slot;
static SemaphoreHandle_t s_mutex;

static void reason_set(char *reason, size_t size, const char *message)
{
    if (reason && size) {
        snprintf(reason, size, "%s", message);
    }
}

void auth_hex_encode(const uint8_t *data, size_t length, char *output)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; ++i) {
        output[i * 2] = digits[data[i] >> 4];
        output[i * 2 + 1] = digits[data[i] & 0x0FU];
    }
    output[length * 2] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool hex_decode_exact(const char *text, size_t byte_length, uint8_t *output)
{
    if (!text || strlen(text) != byte_length * 2U) {
        return false;
    }
    for (size_t i = 0; i < byte_length; ++i) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static bool constant_time_equal(const uint8_t *left, const uint8_t *right, size_t length)
{
    uint8_t difference = 0;
    for (size_t i = 0; i < length; ++i) {
        difference |= left[i] ^ right[i];
    }
    return difference == 0;
}

esp_err_t auth_init(const uint8_t key[FW_AUTH_KEY_BYTES])
{
    if (!key) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_key, key, sizeof(s_key));
    s_mutex = xSemaphoreCreateMutex();
    return s_mutex ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t auth_challenge_issue(char nonce_hex[FW_AUTH_HEX_NONCE_LEN + 1])
{
    if (!nonce_hex || !s_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t nonce[FW_AUTH_NONCE_BYTES];
    esp_fill_random(nonce, sizeof(nonce));
    auth_hex_encode(nonce, sizeof(nonce), nonce_hex);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auth_challenge_t *slot = &s_challenges[s_next_slot++ % AUTH_CHALLENGE_SLOTS];
    slot->valid = true;
    slot->issued_us = esp_timer_get_time();
    memcpy(slot->nonce, nonce_hex, FW_AUTH_HEX_NONCE_LEN + 1);
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool auth_request_verify(const char *method, const char *path, const char *nonce_hex,
    size_t content_length, const char *body_sha256_hex, const char *signature_hex,
    char *reason, size_t reason_size)
{
    uint8_t supplied_signature[32];
    uint8_t body_hash[32];
    if (!s_mutex || !method || !path || !nonce_hex ||
        !hex_decode_exact(body_sha256_hex, sizeof(body_hash), body_hash) ||
        !hex_decode_exact(signature_hex, sizeof(supplied_signature), supplied_signature)) {
        reason_set(reason, reason_size, "malformed authentication headers");
        return false;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    auth_challenge_t *challenge = NULL;
    int64_t now_us = esp_timer_get_time();
    for (unsigned i = 0; i < AUTH_CHALLENGE_SLOTS; ++i) {
        if (s_challenges[i].valid && strcmp(s_challenges[i].nonce, nonce_hex) == 0) {
            challenge = &s_challenges[i];
            break;
        }
    }
    if (!challenge) {
        xSemaphoreGive(s_mutex);
        reason_set(reason, reason_size, "unknown or already-used challenge");
        return false;
    }
    if (now_us - challenge->issued_us >
        (int64_t)FW_AUTH_CHALLENGE_TTL_SECONDS * 1000000LL) {
        challenge->valid = false;
        xSemaphoreGive(s_mutex);
        reason_set(reason, reason_size, "expired challenge");
        return false;
    }

    char canonical[AUTH_CANONICAL_MAX];
    int canonical_length = snprintf(canonical, sizeof(canonical),
        "BACNET-IO-AUTH-V1\n%s\n%s\n%s\n%u\n%s\n", method, path,
        nonce_hex, (unsigned)content_length, body_sha256_hex);
    if (canonical_length < 0 || (size_t)canonical_length >= sizeof(canonical)) {
        xSemaphoreGive(s_mutex);
        reason_set(reason, reason_size, "authentication request too long");
        return false;
    }

    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t expected_signature[32];
    int result = mbedtls_md_hmac(sha256, s_key, sizeof(s_key),
        (const uint8_t *)canonical, (size_t)canonical_length, expected_signature);
    bool valid = result == 0 && constant_time_equal(expected_signature,
        supplied_signature, sizeof(expected_signature));
    if (valid) {
        /* Consume only after a correct signature; failed guesses cannot burn a nonce. */
        challenge->valid = false;
    }
    xSemaphoreGive(s_mutex);
    reason_set(reason, reason_size, valid ? "ok" : "signature mismatch");
    return valid;
}

bool auth_sha256_hex(const uint8_t *data, size_t length,
    char output[FW_AUTH_HEX_SHA256_LEN + 1])
{
    if ((!data && length) || !output) {
        return false;
    }
    uint8_t digest[32];
    if (mbedtls_sha256(data, length, digest, 0) != 0) {
        return false;
    }
    auth_hex_encode(digest, sizeof(digest), output);
    return true;
}
