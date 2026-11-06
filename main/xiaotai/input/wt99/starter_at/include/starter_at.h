#pragma once

#include "esp_err.h"

/* UART0 AT commands are available from boot; the command gate defaults on. */
esp_err_t starter_at_start(void);
/* Unlock Wi-Fi writes only after the COMPANION check has completed. */
void starter_at_companion_ready(void);
