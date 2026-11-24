// SPDX-License-Identifier: MIT
#include "xiaotai/theme_lcd480.h"
#include "lcd480_background.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/theme_lcd.h"

typedef struct {
    xiaotai_theme_lcd_variant_t variant;
    lv_image_dsc_t background, video;
} lcd480_t;

static int match(const xiaotai_display_desc_t *desc)
{
    if (!desc || desc->pixel != XIAOTAI_PIXEL_RGB565 || desc->width < 480 || desc->height < 320 ||
        desc->width <= desc->height || (desc->flags & XIAOTAI_DISPLAY_SLOW_REFRESH)) return 0;
    return desc->width == 480 && desc->height == 320 ? 100 : 60;
}

static void page_built(void *self, const xiaotai_ui_host_t *host, void *page, const xiaotai_ui_layer_t *layer)
{
    if (layer->view.kind != XIAOTAI_VIEW_HOME && layer->view.kind != XIAOTAI_VIEW_CLOCK) return;
    lcd480_t *s = self;
    const xiaotai_display_desc_t *desc = xiaotai_ui_host_display(host);
    if (lcd480_background(&s->background, desc->width, desc->height)) {
        lv_obj_set_style_bg_image_src(page, &s->background, 0);
        lv_obj_set_style_bg_image_opa(page, LV_OPA_COVER, 0);
    }
}

static bool emotion_background(void *self, const xiaotai_view_t *view, uint32_t *rgb)
{
    const lcd480_t *s = self;
    if (view->kind != XIAOTAI_VIEW_HOME || !s->background.data) return false;
    *rgb = LCD480_BACKGROUND_CENTER;
    return true;
}

static void *video_region(void *self, const xiaotai_ui_host_t *host, void *parent)
{
    lcd480_t *s = self;
    const xiaotai_display_desc_t *desc = xiaotai_ui_host_display(host);
    if (!s->video.data) {
        /* A true-size image gives the RGB565 painter its complete destination
         * without a scaling transform or a second full-colour frame. */
        const uint32_t stride = lv_draw_buf_width_to_stride(desc->width, LV_COLOR_FORMAT_A1);
        const uint64_t bytes = (uint64_t)stride * desc->height;
        if (!bytes || stride > UINT16_MAX || bytes > UINT32_MAX || bytes > SIZE_MAX) return NULL;
        const uint8_t *blank = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, (size_t)bytes);
        if (!blank) return NULL;
        s->video = (lv_image_dsc_t){.header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A1,
            .w = desc->width, .h = desc->height, .stride = stride}, .data_size = (uint32_t)bytes, .data = blank};
    }
    lv_obj_t *widget = lv_image_create(parent);
    if (!widget) return NULL;
    lv_image_set_src(widget, &s->video);
    lv_image_set_inner_align(widget, LV_IMAGE_ALIGN_TOP_LEFT);
    lv_obj_set_pos(widget, 0, 0);
    lv_obj_set_size(widget, desc->width, desc->height);
    lv_obj_remove_flag(widget, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return widget;
}

static void drop(void *state)
{
    lcd480_t *s = state;
    if (s->background.data) lv_image_cache_drop(&s->background);
    if (s->video.data) lv_image_cache_drop(&s->video);
    xiaotai_mem_free((void *)s->background.data);
    xiaotai_mem_free((void *)s->video.data);
    xiaotai_mem_free(s);
}

static xiaotai_err_t start(const xiaotai_ui_host_t *host, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    lcd480_t *s = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*s));
    if (!s) return XIAOTAI_ERR_NO_MEM;
    s->variant = (xiaotai_theme_lcd_variant_t){
        .features = XIAOTAI_THEME_LCD_EMOTION | (xiaotai_theme_lcd480.clock_on_home ? XIAOTAI_THEME_LCD_HOME_CLOCK : 0),
        .page_built = page_built, .video_region = video_region, .stop = drop, .self = s,
        .emotion_background = emotion_background};
    const xiaotai_err_t err = xiaotai_theme_lcd_start(host, &s->variant, state);
    if (err != XIAOTAI_OK) drop(s);
    return err;
}

const xiaotai_theme_t xiaotai_theme_lcd480 = {
    .name = XIAOTAI_THEME_LCD480_NAME, .backend = XIAOTAI_UI_BACKEND_LVGL, .match = match, .start = start,
    .render = xiaotai_theme_lcd_render, .tick = xiaotai_theme_lcd_tick, .sleep = xiaotai_theme_lcd_sleep,
    .controls = xiaotai_theme_lcd_controls, .stop = xiaotai_theme_lcd_stop, .report = xiaotai_theme_lcd_report,
    .stack_bytes = XIAOTAI_THEME_LCD_STACK_BYTES, .clock_on_home = true,
};

xiaotai_err_t xiaotai_engine_theme_lcd480_init(void)
{
    return xiaotai_ui_theme_register(&xiaotai_theme_lcd480);
}
