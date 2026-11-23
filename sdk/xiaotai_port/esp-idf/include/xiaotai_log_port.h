// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/port/log.h on ESP-IDF: the IDF log with its runtime level control. */
#include "esp_log.h"

#define XIAOTAI_LOG_PORT(level, tag, ...)                        \
    do {                                                         \
        if ((level) == 'E') ESP_LOGE(tag, __VA_ARGS__);          \
        else if ((level) == 'W') ESP_LOGW(tag, __VA_ARGS__);     \
        else if ((level) == 'I') ESP_LOGI(tag, __VA_ARGS__);     \
        else ESP_LOGD(tag, __VA_ARGS__);                         \
    } while (0)
