#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Draw HH:MM:SS.mmm into an exclusively owned packed O_UYY_E_VYY frame. */
bool video_timestamp_overlay_draw(uint8_t *data, size_t length,
                                  uint16_t width, uint16_t height,
                                  const char *timestamp);
