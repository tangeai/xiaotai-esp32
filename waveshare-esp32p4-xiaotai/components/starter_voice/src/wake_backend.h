#pragma once
#include "esp_err.h"
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t wake_backend_init(void);
esp_err_t wake_backend_run(const int16_t *pcm, float *probability);
void wake_backend_deinit(void);
#ifdef __cplusplus
}
#endif
