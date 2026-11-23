// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
typedef int esp_err_t;
typedef struct esp_netif_obj esp_netif_t;
esp_err_t esp_netif_receive(esp_netif_t *netif, void *buffer, size_t size, void *owner);
