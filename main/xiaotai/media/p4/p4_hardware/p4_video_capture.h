#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t p4_video_capture_init(void);
void p4_video_capture_offer(const uint8_t *data, size_t len, uint16_t w,
                            uint16_t h, bool key, uint32_t generation);
