#pragma once
#include <stdbool.h>
typedef struct test_idf_panel *esp_lcd_panel_handle_t;
typedef struct { int unused; } esp_lcd_dpi_panel_event_data_t;
typedef bool (*fixture_dpi_callback_t)(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t *, void *);
typedef struct {
    fixture_dpi_callback_t on_color_trans_done;
#if defined(UI_TEST_IDF_504) || defined(UI_TEST_IDF_602)
    fixture_dpi_callback_t on_refresh_done;
#else
    /* Pin the 5.5 callback selection to its supported name. The real 5.5
     * header also provides on_refresh_done as a deprecated union alias. */
    fixture_dpi_callback_t on_frame_buf_complete;
#endif
} esp_lcd_dpi_panel_event_callbacks_t;
int esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t panel,
    const esp_lcd_dpi_panel_event_callbacks_t *callbacks, void *context);
