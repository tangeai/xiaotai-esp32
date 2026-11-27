#include "xiaotai_crypto.h"
#include "psa/crypto.h"
#include <string.h>

esp_err_t xiaotai_hmac_sha256(const void *key, size_t key_size,
                            const void *input, size_t input_size, uint8_t output[32])
{
    if (!key || !key_size || (!input && input_size) || !output) return ESP_ERR_INVALID_ARG;
    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    mbedtls_svc_key_id_t id = MBEDTLS_SVC_KEY_ID_INIT;
    psa_set_key_type(&attributes, PSA_KEY_TYPE_HMAC);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_HMAC(PSA_ALG_SHA_256));
    psa_status_t status = psa_crypto_init();
    if (status == PSA_SUCCESS) status = psa_import_key(&attributes, key, key_size, &id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) { memset(output, 0, 32); return ESP_FAIL; }
    size_t size = 0;
    status = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), input, input_size, output, 32, &size);
    psa_status_t cleanup = psa_destroy_key(id);
    if (status != PSA_SUCCESS || cleanup != PSA_SUCCESS || size != 32) {
        memset(output, 0, 32);
        return ESP_FAIL;
    }
    return ESP_OK;
}
