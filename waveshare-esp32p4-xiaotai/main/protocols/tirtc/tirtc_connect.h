#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "tiRTC.h"
#include "tirtc_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TIRTC_CONNECT_TOKEN_MAX_LEN 1536

esp_err_t tirtc_connect_start(const tirtc_session_config_t *config,
                              TIRTCCONNECTCALLBACK callback,
                              void *user_data);
esp_err_t tirtc_connect_start_with_token(const char *remote_device_id,
                                         const char *connect_token,
                                         TIRTCCONNECTCALLBACK callback,
                                         void *user_data);
bool tirtc_connect_is_connecting(void);
/* Cancel the application attempt while keeping the TiRTC listener online. */
bool tirtc_connect_abort_attempt(void);
void tirtc_connect_on_tirtc_started(void);
void tirtc_connect_cancel(void);

#ifdef __cplusplus
}
#endif
