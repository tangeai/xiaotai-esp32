// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
esp_err_t esp_netif_init(void);
void *esp_netif_create_default_wifi_sta(void);
