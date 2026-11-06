#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Read-only firmware/transport check. Never writes the C61 flash. */
esp_err_t companion_bootstrap_run(bool manual_retry);
