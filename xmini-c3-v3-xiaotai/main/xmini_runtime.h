// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

esp_err_t xmini_runtime_init(void);
unsigned xmini_runtime_stack_watermark(void);
bool xmini_runtime_ready(void);
enum { XMINI_WIFI_SETUP_IDLE, XMINI_WIFI_SETUP_WAITING, XMINI_WIFI_SETUP_REBOOTING, XMINI_WIFI_SETUP_ERROR_HELD };
esp_err_t xmini_runtime_wifi_setup(void);
esp_err_t xmini_runtime_wifi_cancel(void);
int xmini_runtime_wifi_phase(void);
int xmini_runtime_wifi_error(void);
