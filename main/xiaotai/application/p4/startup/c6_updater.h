#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Run after display initialization, before Wi-Fi, TiRTC or media sessions. */
esp_err_t c6_updater_run(bool manual_retry);
