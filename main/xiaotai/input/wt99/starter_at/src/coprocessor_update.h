#pragma once
#include <stdbool.h>
#include "esp_err.h"
esp_err_t coprocessor_update_start(const char *url, const char *sha256);
esp_err_t coprocessor_diagnostics_command(const char *command);
esp_err_t coprocessor_log_identity(void);
bool coprocessor_update_busy(void);
