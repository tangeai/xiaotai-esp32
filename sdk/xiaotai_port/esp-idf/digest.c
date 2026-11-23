// SPDX-License-Identifier: MIT
#include "xiaotai/port/sys.h"
#include "xiaotai/port/os.h"
#include "digest_limits.h"
#include "mbedtls/platform_util.h"
#include "psa/crypto.h"
#include <string.h>

#define DIGEST_CHUNK 4096U
#define HMAC_ALGORITHM PSA_ALG_HMAC(PSA_ALG_SHA_256)

static xiaotai_err_t result(uint8_t *out, psa_status_t status, size_t size)
{
    if (status == PSA_SUCCESS && size == XIAOTAI_SHA256_BYTES) return XIAOTAI_OK;
    memset(out, 0, XIAOTAI_SHA256_BYTES);
    return XIAOTAI_FAIL;
}

static xiaotai_err_t invalid(uint8_t *out)
{
    if (out) memset(out, 0, XIAOTAI_SHA256_BYTES);
    return XIAOTAI_ERR_INVALID_ARG;
}

xiaotai_err_t xiaotai_sha256(const void *data, size_t size, uint8_t out[XIAOTAI_SHA256_BYTES])
{
    if (!out || (!data && size) || !digest_length_fits(size, 0)) return invalid(out);
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
    for (const uint8_t *next = data; size && status == PSA_SUCCESS;) {
        size_t part = size > DIGEST_CHUNK ? DIGEST_CHUNK : size;
        status = psa_hash_update(&operation, next, part);
        next += part;
        size -= part;
        if (size && status == PSA_SUCCESS) xiaotai_delay_ms(1);
    }
    size_t written = 0;
    if (status == PSA_SUCCESS) status = psa_hash_finish(&operation, out, XIAOTAI_SHA256_BYTES, &written);
    if (status != PSA_SUCCESS) (void)psa_hash_abort(&operation);
    return result(out, status, written);
}

xiaotai_err_t xiaotai_hmac_sha256(const void *key, size_t key_size, const void *data, size_t size,
                                  uint8_t out[XIAOTAI_SHA256_BYTES])
{
    if (!out || !key || !key_size || !digest_length_fits(key_size, 0) ||
        (!data && size) || !digest_length_fits(size, SHA256_BLOCK_BYTES)) return invalid(out);

    // HMAC reduces keys longer than one hash block; do it before PSA enforces its key-size limit.
    uint8_t reduced[XIAOTAI_SHA256_BYTES];
    if (key_size > SHA256_BLOCK_BYTES) {
        xiaotai_err_t error = xiaotai_sha256(key, key_size, reduced);
        if (error != XIAOTAI_OK) {
            memset(out, 0, XIAOTAI_SHA256_BYTES);
            return error;
        }
        key = reduced;
        key_size = sizeof(reduced);
    }
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, HMAC_ALGORITHM);
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) status = psa_import_key(&attributes, key, key_size, &id);
    if (key == reduced) mbedtls_platform_zeroize(reduced, sizeof(reduced));
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) return result(out, status, 0);

    psa_mac_operation_t operation = PSA_MAC_OPERATION_INIT;
    status = psa_mac_sign_setup(&operation, id, HMAC_ALGORITHM);
    for (const uint8_t *next = data; size && status == PSA_SUCCESS;) {
        size_t part = size > DIGEST_CHUNK ? DIGEST_CHUNK : size;
        status = psa_mac_update(&operation, next, part);
        next += part;
        size -= part;
        if (size && status == PSA_SUCCESS) xiaotai_delay_ms(1);
    }
    size_t written = 0;
    if (status == PSA_SUCCESS) status = psa_mac_sign_finish(&operation, out, XIAOTAI_SHA256_BYTES, &written);
    if (status != PSA_SUCCESS) (void)psa_mac_abort(&operation);
    psa_status_t cleanup = psa_destroy_key(id);
    return result(out, status == PSA_SUCCESS ? cleanup : status, written);
}
