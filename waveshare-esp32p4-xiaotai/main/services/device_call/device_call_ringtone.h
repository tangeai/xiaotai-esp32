#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ordinary device-call ringtone. This service owns only the transient speaker
 * path and must be stopped before call media acquires the audio pipeline. */
esp_err_t device_call_ringtone_start(void);
esp_err_t device_call_ringtone_stop(void);
bool device_call_ringtone_is_active(void);

#ifdef __cplusplus
}
#endif
