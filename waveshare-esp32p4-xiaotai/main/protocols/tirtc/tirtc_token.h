#pragma once

#include <stddef.h>

#include "esp_err.h"

#include "tirtc_session.h"

esp_err_t tirtc_token_fetch_connect(const tirtc_session_config_t *config,
                                           const char *peer_id,
                                           char *out_token,
                                           size_t out_token_size);
