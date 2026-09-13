#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "display.h"

#define WIFI_PAGE_CONNECTION_TEXT_MAX 96
#define WIFI_PAGE_SCAN_TEXT_MAX       32
#define WIFI_PAGE_ROW_TEXT_MAX        48

typedef struct {
    bool visible;
    bool connected;
    char text[WIFI_PAGE_ROW_TEXT_MAX];
} display_wifi_page_row_model_t;

typedef struct {
    char connection_text[WIFI_PAGE_CONNECTION_TEXT_MAX];
    uint32_t connection_color;
    char scan_text[WIFI_PAGE_SCAN_TEXT_MAX];
    uint32_t scan_color;
    uint16_t visible_count;
    display_wifi_page_row_model_t rows[DISPLAY_WIFI_SCAN_MAX];
} wifi_page_model_t;

void wifi_page_model_build(const display_status_t *status,
                                            wifi_page_model_t *model);
bool wifi_page_model_equals(const wifi_page_model_t *lhs,
                                             const wifi_page_model_t *rhs);