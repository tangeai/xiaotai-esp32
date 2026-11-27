// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t cores;
    uint16_t revision;
} esp_chip_info_t;

void esp_chip_info(esp_chip_info_t *chip);
