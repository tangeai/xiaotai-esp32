// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char version[32];
    uint8_t app_elf_sha256[32];
} esp_app_desc_t;

const esp_app_desc_t *esp_app_get_description(void);
int esp_app_get_elf_sha256(char *out, size_t size);
