#pragma once

#include "esp_camera.h"

typedef struct {
    camera_config_t wiring;
    unsigned sensor_pid;
    framesize_t max_frame_size;
} xiaotai_esp32_camera_board_t;

// The selected board passes its wiring to the shared driver.
esp_err_t xiaotai_esp32_camera_open(const xiaotai_esp32_camera_board_t *board,
                                  unsigned width, unsigned height);
