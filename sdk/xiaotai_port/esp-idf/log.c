// SPDX-License-Identifier: MIT
/* The run-time threshold of xiaotai/port/log.h on ESP-IDF. */
#include "xiaotai/port/log.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "sdkconfig.h"

/* The console numbers the levels as ESP-IDF does (ESP_LOG_NONE .. ESP_LOG_DEBUG). */
#define LOG_CLAMP(level) ((level) > XIAOTAI_LOG_LEVEL_DEBUG ? XIAOTAI_LOG_LEVEL_DEBUG : (level))
static atomic_uint s_log_level = LOG_CLAMP(CONFIG_LOG_DEFAULT_LEVEL);

xiaotai_err_t xiaotai_log_set_level(uint8_t level)
{
    if (level > XIAOTAI_LOG_LEVEL_DEBUG) return XIAOTAI_ERR_INVALID_ARG;
    esp_log_level_set("*", (esp_log_level_t)level);
    atomic_store(&s_log_level, level);
    return XIAOTAI_OK;
}

uint8_t xiaotai_log_level(void) { return (uint8_t)atomic_load(&s_log_level); }
uint8_t xiaotai_log_level_max(void) { return (uint8_t)LOG_CLAMP(CONFIG_LOG_MAXIMUM_LEVEL); }
