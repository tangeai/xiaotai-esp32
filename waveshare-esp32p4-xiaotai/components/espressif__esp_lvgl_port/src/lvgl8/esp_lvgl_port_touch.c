/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

static const char *TAG = "LVGL";

/*******************************************************************************
* Types definitions
*******************************************************************************/

typedef struct {
    esp_lcd_touch_handle_t   handle;     /* LCD touch IO handle */
    lv_indev_drv_t           indev_drv;  /* LVGL input device driver */
    struct {
        float x;
        float y;
    } scale;                            /* Touch scale */
    int64_t last_poll_us;
    int64_t stats_start_us;
    uint32_t max_poll_gap_us;
    uint32_t max_read_us;
    uint32_t delayed_polls;
    uint32_t read_errors;
    uint32_t press_edges;
    bool was_pressed;
} lvgl_port_touch_ctx_t;

/*******************************************************************************
* Function definitions
*******************************************************************************/

static void lvgl_port_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data);

/*******************************************************************************
* Public API functions
*******************************************************************************/

lv_indev_t *lvgl_port_add_touch(const lvgl_port_touch_cfg_t *touch_cfg)
{
    assert(touch_cfg != NULL);
    assert(touch_cfg->disp != NULL);
    assert(touch_cfg->handle != NULL);

    /* Touch context */
    lvgl_port_touch_ctx_t *touch_ctx = calloc(1, sizeof(lvgl_port_touch_ctx_t));
    if (touch_ctx == NULL) {
        ESP_LOGE(TAG, "Not enough memory for touch context allocation!");
        return NULL;
    }
    touch_ctx->handle = touch_cfg->handle;
    touch_ctx->scale.x = (touch_cfg->scale.x ? touch_cfg->scale.x : 1);
    touch_ctx->scale.y = (touch_cfg->scale.y ? touch_cfg->scale.y : 1);

    /* Register a touchpad input device */
    lv_indev_drv_init(&touch_ctx->indev_drv);
    touch_ctx->indev_drv.type = LV_INDEV_TYPE_POINTER;
    touch_ctx->indev_drv.disp = touch_cfg->disp;
    touch_ctx->indev_drv.read_cb = lvgl_port_touchpad_read;
    touch_ctx->indev_drv.user_data = touch_ctx;
    return lv_indev_drv_register(&touch_ctx->indev_drv);
}

esp_err_t lvgl_port_remove_touch(lv_indev_t *touch)
{
    assert(touch);
    lv_indev_drv_t *indev_drv = touch->driver;
    assert(indev_drv);
    lvgl_port_touch_ctx_t *touch_ctx = (lvgl_port_touch_ctx_t *)indev_drv->user_data;

    /* Remove input device driver */
    lv_indev_delete(touch);

    if (touch_ctx) {
        free(touch_ctx);
    }

    return ESP_OK;
}

/*******************************************************************************
* Private functions
*******************************************************************************/

static void lvgl_port_touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    assert(indev_drv);
    lvgl_port_touch_ctx_t *touch_ctx = (lvgl_port_touch_ctx_t *)indev_drv->user_data;
    assert(touch_ctx->handle);

    esp_lcd_touch_point_data_t touchpad_data = {0};
    uint8_t touchpad_cnt = 0;
    int64_t poll_start_us = esp_timer_get_time();
    if (touch_ctx->stats_start_us == 0) {
        touch_ctx->stats_start_us = poll_start_us;
    }
    if (touch_ctx->last_poll_us > 0) {
        uint32_t gap_us = (uint32_t)(poll_start_us - touch_ctx->last_poll_us);
        if (gap_us > touch_ctx->max_poll_gap_us) {
            touch_ctx->max_poll_gap_us = gap_us;
        }
        if (gap_us > 45000U) {
            touch_ctx->delayed_polls++;
        }
    }
    touch_ctx->last_poll_us = poll_start_us;

    /* Read data from touch controller into memory */
    esp_err_t ret = esp_lcd_touch_read_data(touch_ctx->handle);

    /* Read data from touch controller */
    if (ret == ESP_OK) {
        ret = esp_lcd_touch_get_data(touch_ctx->handle, &touchpad_data, &touchpad_cnt, 1);
    }

    if (ret == ESP_OK && touchpad_cnt > 0) {
        data->point.x = touch_ctx->scale.x * touchpad_data.x;
        data->point.y = touch_ctx->scale.y * touchpad_data.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    if (ret != ESP_OK) {
        touch_ctx->read_errors++;
    }
    bool pressed = ret == ESP_OK && touchpad_cnt > 0;
    if (pressed && !touch_ctx->was_pressed) {
        touch_ctx->press_edges++;
    }
    touch_ctx->was_pressed = pressed;
    uint32_t read_us = (uint32_t)(esp_timer_get_time() - poll_start_us);
    if (read_us > touch_ctx->max_read_us) {
        touch_ctx->max_read_us = read_us;
    }
    if (poll_start_us - touch_ctx->stats_start_us >= 10000000LL) {
        if (touch_ctx->delayed_polls != 0 || touch_ctx->read_errors != 0 ||
            touch_ctx->press_edges != 0) {
            ESP_LOG_LEVEL_LOCAL(touch_ctx->delayed_polls != 0 || touch_ctx->read_errors != 0 ?
                                    ESP_LOG_WARN : ESP_LOG_INFO,
                                TAG,
                                "touch poll: gap_max=%luus late=%lu read_max=%luus err=%lu press=%lu",
                                (unsigned long)touch_ctx->max_poll_gap_us,
                                (unsigned long)touch_ctx->delayed_polls,
                                (unsigned long)touch_ctx->max_read_us,
                                (unsigned long)touch_ctx->read_errors,
                                (unsigned long)touch_ctx->press_edges);
        }
        touch_ctx->stats_start_us = poll_start_us;
        touch_ctx->max_poll_gap_us = 0;
        touch_ctx->max_read_us = 0;
        touch_ctx->delayed_polls = 0;
        touch_ctx->read_errors = 0;
        touch_ctx->press_edges = 0;
    }
}
