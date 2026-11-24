// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/port/log.h on ESP-IDF. XiaoTai's lines pass XiaoTai's own gate
 * (xiaotai_log_enabled) and are then handed to the ESP-IDF log as finished
 * text, in the layout ESP-IDF itself prints, so they follow whatever sink the
 * application installed. They are written under one fixed ESP-IDF tag that the
 * port keeps open: ESP-IDF's own threshold ("*", the system threshold here)
 * therefore filters the platform's lines only, and the lines exist in the
 * build whatever CONFIG_LOG_MAXIMUM_LEVEL says. */
#include <inttypes.h>
#include "esp_log.h"

extern const char xiaotai_log_gate_tag[];

#define XIAOTAI_LOG_PORT(level, letter, tag, format, ...)                                               \
    do {                                                                                                \
        if (xiaotai_log_enabled((level), (tag)))                                                        \
            esp_log_write(ESP_LOG_ERROR, xiaotai_log_gate_tag, letter " (%" PRIu32 ") %s: " format "\n", \
                          esp_log_timestamp(), (tag), ##__VA_ARGS__);                                   \
    } while (0)
