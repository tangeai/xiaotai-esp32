#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t screen_debug_server_start(void);
void screen_debug_server_stop(void);
bool screen_debug_server_is_running(void);
