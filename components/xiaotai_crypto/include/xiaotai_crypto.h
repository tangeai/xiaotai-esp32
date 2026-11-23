#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t xiaotai_hmac_sha256(const void *key, size_t key_size,
                            const void *input, size_t input_size, uint8_t output[32]);
#ifdef __cplusplus
}
#endif
