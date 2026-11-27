#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xiaotai_display_handles {
    struct esp_lcd_panel_io_t *panel_io;
    struct esp_lcd_panel_t *panel;
    struct esp_lcd_touch_s *touch;
    uint32_t transfer_pixels;
    // Transform from the logical viewport to the configured panel orientation.
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool swap_rgb565_bytes;
    // Render into one full PSRAM frame instead of a small DMA partial buffer:
    // for panels whose partial areas were not always committed.
    bool full_frame_buffer;
} xiaotai_display_handles_t;

// Called by a single startup owner. Publish handles only after successful init.
// The board owns these resources; LVGL borrows them for the life of the display.
esp_err_t xiaotai_board_display_open(xiaotai_display_handles_t *handles);
esp_err_t xiaotai_board_backlight_set(bool awake);

#ifdef __cplusplus
}
#endif
