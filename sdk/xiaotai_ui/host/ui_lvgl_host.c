// SPDX-License-Identifier: MIT
#include "lvgl/ui_lvgl_private.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"

#include <string.h>

enum { HOST_STRIP_ROWS = 16, HOST_PREVIEW_DISPLAYS = 2 };

/* Host only: what each display shows, as RGB888, so preview tools can save a
 * picture of any scene, also while a session runs (xiaotai_ui_capture() is
 * refused then). Firmware builds do not have it. */
static struct {
    uint16_t width, height;
    uint8_t *rgb;
} s_preview[HOST_PREVIEW_DISPLAYS];

static void preview_store(const ui_display_t *display, const lv_area_t *area, const uint8_t *data, uint8_t bpp)
{
    const uint8_t index = display->host.index;
    const xiaotai_display_desc_t *desc = &display->part->desc;
    if (index >= HOST_PREVIEW_DISPLAYS) return;
    if (!s_preview[index].rgb || s_preview[index].width != desc->width || s_preview[index].height != desc->height) {
        xiaotai_mem_free(s_preview[index].rgb);
        s_preview[index].rgb = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, (size_t)desc->width * desc->height, 3);
        if (!s_preview[index].rgb) return;
        s_preview[index].width = desc->width;
        s_preview[index].height = desc->height;
    }
    const int32_t width = lv_area_get_width(area);
    for (int32_t y = area->y1; y <= area->y2; ++y) {
        const uint8_t *src = data + (size_t)(y - area->y1) * (size_t)width * bpp;
        uint8_t *dst = s_preview[index].rgb + ((size_t)y * desc->width + (size_t)area->x1) * 3U;
        for (int32_t x = 0; x < width; ++x, src += bpp, dst += 3) {
            if (bpp == 2) {
                const uint16_t px = (uint16_t)(src[0] | (src[1] << 8));
                dst[0] = (uint8_t)(((px >> 11) & 0x1fU) * 255U / 31U);
                dst[1] = (uint8_t)(((px >> 5) & 0x3fU) * 255U / 63U);
                dst[2] = (uint8_t)((px & 0x1fU) * 255U / 31U);
            } else {
                dst[0] = src[2];   /* LVGL RGB888 is stored blue, green, red */
                dst[1] = src[1];
                dst[2] = src[0];
            }
        }
    }
}

bool xiaotai_ui_host_preview(uint8_t display, uint16_t *width, uint16_t *height, const uint8_t **rgb888)
{
    if (display >= HOST_PREVIEW_DISPLAYS || !s_preview[display].rgb) return false;
    *width = s_preview[display].width;
    *height = s_preview[display].height;
    *rgb888 = s_preview[display].rgb;
    return true;
}

static xiaotai_rect_t native_area(const xiaotai_display_desc_t *desc, xiaotai_rect_t a)
{
    switch (desc->rotation) {
        case 1: return (xiaotai_rect_t){desc->height - a.y - a.height, a.x, a.height, a.width};
        case 2: return (xiaotai_rect_t){desc->width - a.x - a.width, desc->height - a.y - a.height, a.width, a.height};
        case 3: return (xiaotai_rect_t){a.y, desc->width - a.x - a.width, a.height, a.width};
        default: return a;
    }
}

static void flush(lv_display_t *root, const lv_area_t *lv_area, uint8_t *data)
{
    ui_display_t *display = lv_display_get_user_data(root);
    const xiaotai_display_desc_t *desc = &display->part->desc;
    const uint16_t width = (uint16_t)lv_area_get_width(lv_area);
    const uint16_t height = (uint16_t)lv_area_get_height(lv_area);
    const uint8_t bpp = desc->pixel == XIAOTAI_PIXEL_RGB565 ? 2 : 3;
    if (lv_area->x1 >= 0 && lv_area->y1 >= 0 && lv_area->x2 < desc->width &&
        lv_area->y2 < desc->height && width && height) preview_store(display, lv_area, data, bpp);
    if (lv_area->x1 >= 0 && lv_area->y1 >= 0 && lv_area->x2 < desc->width &&
        lv_area->y2 < desc->height && width && height &&
        !ui_lvgl_capture_flush(display, lv_area, data, (size_t)width * bpp)) {
        xiaotai_rect_t area = {.x = (uint16_t)lv_area->x1, .y = (uint16_t)lv_area->y1,
                               .width = width, .height = height};
        const uint32_t pixels = (uint32_t)width * height;
        uint8_t *out = data;
        if (desc->rotation) {
            /* Same native mapping as the firmware port, rotated out of place. */
            out = lv_display_get_driver_data(root);
            area = native_area(desc, area);
            for (size_t y = 0; y < height; ++y)
                for (size_t x = 0; x < width; ++x) {
                    const size_t at = desc->rotation == 2U ? (size_t)pixels - 1U - (y * width + x)
                        : desc->rotation == 1U ? x * height + (height - 1U - y) : (width - 1U - x) * height + y;
                    memcpy(out + at * bpp, data + (y * width + x) * bpp, bpp);
                }
        }
        const bool swap = bpp == 2 && (desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES);
        if (swap) lv_draw_sw_rgb565_swap(out, pixels);
        const int64_t start_us = xiaotai_time_us();
        const xiaotai_err_t err = display->part->present(display->part->self, &area, out);
        const int64_t elapsed_us = xiaotai_time_us() - start_us;
        if (swap) lv_draw_sw_rgb565_swap(out, pixels);
        ui_count_flush(display, pixels, (size_t)pixels * bpp, elapsed_us > 0 ? (uint64_t)elapsed_us : 0);
        if (err != XIAOTAI_OK) {
            (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
            ++display->frame.errors;
            xiaotai_mutex_unlock(&g_ui.lock);
        }
    }
    lv_display_flush_ready(root);
}

bool ui_lvgl_port_push(ui_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const xiaotai_rect_t rect = {.x = (uint16_t)area->x1, .y = (uint16_t)area->y1,
        .width = (uint16_t)lv_area_get_width(area), .height = (uint16_t)lv_area_get_height(area)};
    const uint32_t count = (uint32_t)rect.width * rect.height;
    const bool swap = (display->part->desc.flags & XIAOTAI_DISPLAY_SWAP_BYTES) != 0;
    if (swap) lv_draw_sw_rgb565_swap(pixels, count);
    preview_store(display, area, pixels, 2);
    if (swap) lv_draw_sw_rgb565_swap(pixels, count);
    const xiaotai_err_t err = display->part->present(display->part->self, &rect, pixels);
    ui_count_flush(display, count, (size_t)count * 2U, 0);
    return err == XIAOTAI_OK;
}

xiaotai_err_t ui_lvgl_port_open(ui_display_t *display, lv_display_t **out)
{
    *out = NULL;
    const xiaotai_display_desc_t *desc = &display->part->desc;
    if (desc->rotation > 3U || (desc->pixel != XIAOTAI_PIXEL_RGB565 &&
                                desc->pixel != XIAOTAI_PIXEL_RGB888)) return XIAOTAI_ERR_NOT_SUPPORTED;
    const uint32_t bpp = desc->pixel == XIAOTAI_PIXEL_RGB565 ? 2U : 3U;
    uint32_t rows = desc->height;
    const bool partial = desc->render == XIAOTAI_RENDER_PARTIAL;
    if (partial && rows > HOST_STRIP_ROWS) rows = HOST_STRIP_ROWS;
    const uint64_t bytes = (uint64_t)desc->width * rows * bpp;
    if (!bytes || bytes > UINT32_MAX || bytes > SIZE_MAX) return XIAOTAI_ERR_INVALID_SIZE;
    const size_t alignment = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
    void *pixels = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, (size_t)bytes);
    if (!pixels) return XIAOTAI_ERR_NO_MEM;
    void *rotated = desc->rotation ? xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, (size_t)bytes) : NULL;
    lv_display_t *root = desc->rotation && !rotated ? NULL : lv_display_create(desc->width, desc->height);
    if (!root) {
        xiaotai_mem_free(rotated);
        xiaotai_mem_free(pixels);
        return XIAOTAI_ERR_NO_MEM;
    }
    lv_display_set_driver_data(root, rotated);
    lv_display_set_color_format(root, bpp == 2 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB888);
    lv_display_set_buffers(root, pixels, NULL, (uint32_t)bytes,
                           partial ? LV_DISPLAY_RENDER_MODE_PARTIAL : LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(root, display);
    lv_display_set_flush_cb(root, flush);
    lv_timer_pause(lv_display_get_refr_timer(root)); /* pumped on this display's task */
    *out = root;
    return XIAOTAI_OK;
}

void ui_lvgl_port_close(ui_display_t *display, lv_display_t *root)
{
    void *pixels = lv_display_get_buf_active(root)->data;
    void *rotated = lv_display_get_driver_data(root);
    lv_display_delete(root);
    xiaotai_mem_free(rotated);
    xiaotai_mem_free(pixels);
    const uint8_t index = display->host.index;
    if (index < HOST_PREVIEW_DISPLAYS) {
        xiaotai_mem_free(s_preview[index].rgb);
        s_preview[index].rgb = NULL;
    }
}

bool ui_lvgl_port_ready(lv_display_t *root)
{
    (void)root;
    return true;
}

void ui_lvgl_port_settle(lv_display_t *root) { (void)root; }

void ui_lvgl_port_reset(void) {}

void ui_lvgl_port_report(char *words, size_t capacity)
{
    if (words && capacity) words[0] = '\0';
}

xiaotai_err_t ui_lvgl_port_accelerate(bool on)
{
    (void)on;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
