#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t xiaotai_startup_initialize_platform(void);
esp_err_t xiaotai_startup_launch_services(void);
void xiaotai_startup_log_heap(const char* stage);

#ifdef __cplusplus
}
#endif
