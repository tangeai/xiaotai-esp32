#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*system_time_sync_cb_t)(esp_err_t result, bool time_valid, void *ctx);

bool system_time_has_valid_time(void);
void system_time_set_sync_cb(system_time_sync_cb_t cb, void *ctx);
esp_err_t system_time_request_sync(bool force_sync);
esp_err_t system_time_once(bool force_sync);

#ifdef __cplusplus
}
#endif
