#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "starter_tirtc.h"

/* Runtime/media owner only. A dormant worker is retained in PSRAM after first
 * use. Sensor, frame, encoder and DMA resources exist only for an admitted H5
 * video subscription or an admitted AI visual session. */
esp_err_t starter_camera_prepare(starter_tirtc_mode_t mode, uint32_t generation);
void starter_camera_set_session(starter_tirtc_mode_t mode, uint32_t generation);

/* Only the camera worker uses these board hooks. They share the existing
 * output mutex for the short PCA9557 transaction, not capture or encoding. */
esp_err_t starter_media_camera_power(bool enabled);
bool starter_media_camera_current(starter_tirtc_mode_t mode, uint32_t generation);
