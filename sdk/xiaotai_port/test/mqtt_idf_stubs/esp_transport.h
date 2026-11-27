// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
typedef struct esp_transport_item_t *esp_transport_handle_t;
typedef struct {
    bool keep_alive_enable;
    int keep_alive_idle, keep_alive_interval, keep_alive_count;
} esp_transport_keep_alive_t;
