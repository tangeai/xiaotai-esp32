#pragma once

#include "esp_log.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Detailed flow logs are available for focused diagnosis without polluting the
 * normal realtime path. Warnings and errors must never use this macro. */
#if CONFIG_APP_VERBOSE_RUNTIME_LOGS
#define APP_LOG_DETAIL(tag, format, ...) ESP_LOGI(tag, format, ##__VA_ARGS__)
#else
#define APP_LOG_DETAIL(tag, format, ...)            \
    do {                                             \
        if (0) {                                     \
            ESP_LOGI(tag, format, ##__VA_ARGS__);    \
        }                                            \
    } while (0)
#endif

#ifdef __cplusplus
}
#endif
