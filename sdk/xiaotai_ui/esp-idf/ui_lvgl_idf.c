// SPDX-License-Identifier: MIT
#include "lvgl/ui_lvgl_private.h"
#include "xiaotai/port/mem.h"

#include <string.h>

#include "soc/soc_caps.h"
#if SOC_MIPI_DSI_SUPPORTED
#include <stdatomic.h>
#include "esp_attr.h"
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SCANOUT_SLOTS 4U
#define SCANOUT_WAIT_MS 100U

/* The ISR never refers to a UI display or heap object. A late callback can
 * only see a static slot, even when IDF is replacing the callback and ctx. */
typedef struct {
    _Atomic(esp_lcd_panel_handle_t) panel;
    StaticSemaphore_t storage;
    SemaphoreHandle_t done;
} scanout_irq_t;

static scanout_irq_t scanout_irqs[SCANOUT_SLOTS];

static bool IRAM_ATTR scanout_completed(esp_lcd_panel_handle_t panel,
                                        esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    (void)event;
    scanout_irq_t *slot = context;
    if (!slot || atomic_load_explicit(&slot->panel, memory_order_acquire) != panel) return false;
    BaseType_t wake = pdFALSE;
    (void)xSemaphoreGiveFromISR(slot->done, &wake);
    return wake == pdTRUE;
}

static xiaotai_err_t scanout_attach(esp_lcd_panel_handle_t panel, scanout_irq_t **out)
{
    scanout_irq_t *slot = NULL;
    for (size_t i = 0; i < SCANOUT_SLOTS; ++i) {
        const esp_lcd_panel_handle_t used = atomic_load(&scanout_irqs[i].panel);
        if (used == panel) return XIAOTAI_ERR_INVALID_STATE;
        if (!used && !slot) slot = &scanout_irqs[i];
    }
    if (!slot) return XIAOTAI_ERR_NO_MEM;
    if (!slot->done) slot->done = xSemaphoreCreateBinaryStatic(&slot->storage);
    if (!slot->done) return XIAOTAI_ERR_NO_MEM;
    (void)xSemaphoreTake(slot->done, 0);
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) || \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 3)
        .on_frame_buf_complete = scanout_completed,
#else
        .on_refresh_done = scanout_completed,
#endif
    };
    atomic_store(&slot->panel, panel);
    const xiaotai_err_t err = esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, slot);
    if (err != XIAOTAI_OK) atomic_store(&slot->panel, NULL);
    else *out = slot;
    return err;
}

static xiaotai_err_t scanout_detach(scanout_irq_t *slot)
{
    const esp_lcd_panel_handle_t panel = atomic_load(&slot->panel);
    const esp_lcd_dpi_panel_event_callbacks_t callbacks = {0};
    const xiaotai_err_t err = esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, NULL);
    if (err == XIAOTAI_OK) atomic_store(&slot->panel, NULL);
    return err;
}
#endif

typedef struct {
    ui_display_t *display;
    void *pixels;
    uint8_t *visited;
    uint8_t next_scanout;
    bool pending, submitted;
    lv_draw_buf_t scanout[2];
#if SOC_MIPI_DSI_SUPPORTED
    scanout_irq_t *irq;
#endif
} native_t;

static void note_error(ui_display_t *display)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    ++display->frame.errors;
    xiaotai_mutex_unlock(&g_ui.lock);
}

#if SOC_MIPI_DSI_SUPPORTED
static void scanout_fault(native_t *port, bool fault)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    port->display->frame.fault = fault;
    xiaotai_mutex_unlock(&g_ui.lock);
}

static void scanout_settled(native_t *port)
{
    port->pending = false;
    if (port->submitted) port->next_scanout ^= 1U;
    scanout_fault(port, false);
}
#endif

/* The logical and native views contain the same number of pixels. Permute
 * complete rotation cycles in place; only a bit per pixel is needed. */
static void rotate(uint8_t *data, uint8_t *visited, size_t w, size_t h, size_t bpp, unsigned turns)
{
    const size_t pixels = w * h;
    if (turns == 2U) {
        for (size_t i = 0; i < pixels / 2U; ++i)
            for (size_t b = 0; b < bpp; ++b) {
                const size_t a = i * bpp + b, z = (pixels - 1U - i) * bpp + b;
                const uint8_t tmp = data[a];
                data[a] = data[z];
                data[z] = tmp;
            }
        return;
    }
    memset(visited, 0, (pixels + 7U) / 8U);
    for (size_t i = 0; i < pixels; ++i) {
        if (visited[i / 8U] & (1U << (i % 8U))) continue;
        uint8_t sample[3];
        memcpy(sample, data + i * bpp, bpp);
        size_t at = i;
        do {
            visited[at / 8U] |= (uint8_t)(1U << (at % 8U));
            const size_t x = at % w, y = at / w;
            at = turns == 1U ? x * h + (h - 1U - y) : (w - 1U - x) * h + y;
            for (size_t b = 0; b < bpp; ++b) {
                const size_t offset = at * bpp + b;
                const uint8_t tmp = data[offset];
                data[offset] = sample[b];
                sample[b] = tmp;
            }
        } while (at != i);
    }
}

static xiaotai_rect_t native_area(const xiaotai_display_desc_t *desc, xiaotai_rect_t a)
{
    switch (desc->rotation) {
        case 1: return (xiaotai_rect_t){desc->height - a.y - a.height, a.x, a.height, a.width};
        case 2: return (xiaotai_rect_t){desc->width - a.x - a.width, desc->height - a.y - a.height,
                                       a.width, a.height};
        case 3: return (xiaotai_rect_t){a.y, desc->width - a.x - a.width, a.height, a.width};
        default: return a;
    }
}

static xiaotai_err_t send_scanout(native_t *port, const xiaotai_rect_t *area, const void *pixels)
{
#if SOC_MIPI_DSI_SUPPORTED
    ui_display_t *display = port->display;
    port->pending = true;
    const xiaotai_err_t err = display->part->present(display->part->self, area, pixels);
    port->submitted = err == XIAOTAI_OK;
    /* Discard the old frame's signal, including one from before submission.
     * A fresh frame-buffer completion means GDMA has moved onto this frame. */
    (void)xSemaphoreTake(port->irq->done, 0);
    if (!xSemaphoreTake(port->irq->done, pdMS_TO_TICKS(SCANOUT_WAIT_MS))) {
        scanout_fault(port, true);
        return err != XIAOTAI_OK ? err : XIAOTAI_ERR_TIMEOUT;
    }
    scanout_settled(port);
    return err;
#else
    (void)port;
    (void)area;
    (void)pixels;
    return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
}

bool ui_lvgl_port_ready(lv_display_t *root)
{
    native_t *port = lv_display_get_driver_data(root);
    const xiaotai_display_desc_t *desc = &port->display->part->desc;
    if (desc->render == XIAOTAI_RENDER_SCANOUT && desc->buffers == 2U) {
#if SOC_MIPI_DSI_SUPPORTED
        if (port->pending) {
            if (!xSemaphoreTake(port->irq->done, 0)) return false;
            scanout_settled(port);
        }
#endif
        lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout], NULL);
    }
    return true;
}

static void flush(lv_display_t *root, const lv_area_t *area, uint8_t *data)
{
    native_t *port = lv_display_get_driver_data(root);
    ui_display_t *display = port->display;
    const xiaotai_display_desc_t *desc = &display->part->desc;
    const lv_draw_buf_t *source = lv_display_get_buf_active(root);
    const size_t stride = source->header.stride;
    if (ui_lvgl_capture_flush(display, area, data, stride)) goto done;
    if (area->x1 < 0 || area->y1 < 0 || area->x2 < area->x1 || area->y2 < area->y1 ||
        area->x2 >= desc->width || area->y2 >= desc->height) {
        note_error(display);
        goto done;
    }
    const xiaotai_rect_t logical = {area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area)};
    const xiaotai_rect_t native = native_area(desc, logical);
    const size_t bpp = desc->pixel == XIAOTAI_PIXEL_RGB565 ? 2U : 3U;
    const size_t packed = (size_t)logical.width * bpp;
    if (stride < packed || (size_t)(logical.height - 1U) * stride + packed > source->data_size) {
        note_error(display);
        goto done;
    }
    if (stride != packed) {
        for (size_t y = 1; y < logical.height; ++y)
            memmove(data + y * packed, data + y * stride, packed);
    }
    if (desc->rotation) rotate(data, port->visited, logical.width, logical.height, bpp, desc->rotation);
    const uint32_t pixels = (uint32_t)logical.width * logical.height;
    const bool swap = (desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES) != 0;
    if (swap) lv_draw_sw_rgb565_swap(data, pixels);
    const int64_t start = xiaotai_time_us();
    /* HAL present owns any SPI/DMA fence. In particular it cannot return
     * with the renderer's memory still in use by a partial transfer. */
    const xiaotai_err_t err = desc->render == XIAOTAI_RENDER_SCANOUT && desc->buffers == 2U
        ? send_scanout(port, &native, data) : display->part->present(display->part->self, &native, data);
    const int64_t duration = xiaotai_time_us() - start;
    ui_count_flush(display, pixels, (size_t)pixels * bpp, duration > 0 ? (uint64_t)duration : 0);
    if (err != XIAOTAI_OK) note_error(display);
    if (swap) lv_draw_sw_rgb565_swap(data, pixels);
    if (desc->rotation) rotate(data, port->visited, native.width, native.height, bpp, (4U - desc->rotation) % 4U);
    if (stride != packed) {
        for (size_t y = logical.height; y-- > 1U;) memmove(data + y * stride, data + y * packed, packed);
    }
done:
    lv_display_flush_ready(root);
}

static void free_port(native_t *port)
{
#if SOC_MIPI_DSI_SUPPORTED
    if (port->irq) {
        const xiaotai_err_t err = scanout_detach(port->irq);
        if (err != XIAOTAI_OK) port->display->native_cleanup_error = err;
    }
#endif
    xiaotai_mem_free(port->visited);
    xiaotai_mem_free(port->pixels);
    xiaotai_mem_free(port);
}

xiaotai_err_t ui_lvgl_port_open(ui_display_t *display, lv_display_t **out)
{
    *out = NULL;
    const xiaotai_display_desc_t *desc = &display->part->desc;
    const bool partial = desc->render == XIAOTAI_RENDER_PARTIAL;
    const bool scanout = desc->render == XIAOTAI_RENDER_SCANOUT;
    const bool rgb565 = desc->pixel == XIAOTAI_PIXEL_RGB565;
    const size_t bpp = rgb565 ? 2U : 3U;
    if ((desc->pixel != XIAOTAI_PIXEL_RGB888 && !rgb565) || desc->rotation > 3U ||
        (desc->render != XIAOTAI_RENDER_FULL && !partial && !scanout) ||
        ((desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES) && !rgb565) || (scanout &&
        (!rgb565 || desc->rotation || (desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES))))
        return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!desc->width || !desc->height ||
        ((desc->flags & XIAOTAI_DISPLAY_EVEN_AREAS) && (desc->rotation & 1U) && (partial || (desc->height & 1U))))
        return XIAOTAI_ERR_INVALID_SIZE;
    const size_t rows = partial && desc->transfer_pixels
        ? desc->transfer_pixels / desc->width : desc->height;
    const size_t used_rows = rows < desc->height ? rows : desc->height;
    const lv_color_format_t format = rgb565 ? LV_COLOR_FORMAT_RGB565 : LV_COLOR_FORMAT_RGB888;
    const uint32_t stride = partial ? lv_draw_buf_width_to_stride(desc->width, format) : (uint32_t)desc->width * bpp;
    const uint64_t bytes = (uint64_t)stride * used_rows;
    if (!bytes || bytes > UINT32_MAX || bytes > SIZE_MAX ||
        (desc->buffer_align && (desc->buffer_align & (desc->buffer_align - 1U)))) return XIAOTAI_ERR_INVALID_SIZE;
    if (scanout && (!desc->buffers || desc->buffers > 2U || !display->handles.framebuffers[0] ||
        (desc->buffers == 2U && (!display->handles.panel || !display->handles.framebuffers[1] ||
          display->handles.framebuffers[0] == display->handles.framebuffers[1])))) return XIAOTAI_ERR_INVALID_ARG;
    if (scanout && desc->buffers == 2U && display->handles.scanout_kind != XIAOTAI_SCANOUT_DPI)
        return XIAOTAI_ERR_NOT_SUPPORTED;
#if !SOC_MIPI_DSI_SUPPORTED
    if (scanout && desc->buffers == 2U) return XIAOTAI_ERR_NOT_SUPPORTED;
#endif
    native_t *port = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*port));
    if (!port) return XIAOTAI_ERR_NO_MEM;
    port->display = display;
    const size_t min_align = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
    const size_t align = desc->buffer_align > min_align ? desc->buffer_align : min_align;
    xiaotai_err_t err = XIAOTAI_ERR_NO_MEM;
    if (!scanout) {
        const xiaotai_mem_kind_t memory = partial && (desc->flags & XIAOTAI_DISPLAY_DMA_BUFFER)
            ? XIAOTAI_MEM_DMA : XIAOTAI_MEM_BULK;
        port->pixels = xiaotai_mem_aligned(memory, align, (size_t)bytes);
        if (!port->pixels) goto failed;
        if (desc->rotation & 1U) {
            /* A partial LVGL strip can be narrower than the display, but it
             * is still bounded by this pixel capacity. */
            port->visited = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, ((size_t)bytes / bpp + 7U) / 8U);
            if (!port->visited) goto failed;
        }
    } else {
        err = XIAOTAI_ERR_INVALID_SIZE;
        for (size_t i = 0; i < desc->buffers; ++i) {
            void *fb = display->handles.framebuffers[i];
            if ((uintptr_t)fb % align || lv_draw_buf_init(&port->scanout[i], desc->width, desc->height,
                                                          format, stride, fb, (uint32_t)bytes) != LV_RESULT_OK)
                goto failed;
        }
    }
    err = XIAOTAI_ERR_NO_MEM;
    lv_display_t *root = lv_display_create(desc->width, desc->height);
    if (!root) goto failed;
    lv_display_set_color_format(root, format);
    lv_display_set_render_mode(root, partial ? LV_DISPLAY_RENDER_MODE_PARTIAL : LV_DISPLAY_RENDER_MODE_FULL);
    if (scanout) lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout], NULL);
    else lv_display_set_buffers_with_stride(root, port->pixels, NULL, (uint32_t)bytes, stride,
                      partial ? LV_DISPLAY_RENDER_MODE_PARTIAL : LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_driver_data(root, port);
    lv_display_set_flush_cb(root, flush);
    lv_timer_pause(lv_display_get_refr_timer(root));
#if SOC_MIPI_DSI_SUPPORTED
    if (scanout && desc->buffers == 2U) {
        err = scanout_attach(display->handles.panel, &port->irq);
        if (err == XIAOTAI_OK) {
            /* Parts may already be open, with either buffer live. Retire it
             * before letting LVGL draw into a known inactive buffer. */
            const xiaotai_rect_t full = {.width = desc->width, .height = desc->height};
            err = send_scanout(port, &full, display->handles.framebuffers[0]);
        }
        if (err != XIAOTAI_OK) {
            lv_display_delete(root);
            goto failed;
        }
        lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout], NULL);
    }
#endif
    *out = root;
    return XIAOTAI_OK;
failed:
    free_port(port);
    return err;
}

void ui_lvgl_port_close(ui_display_t *display, lv_display_t *root)
{
    (void)display;
    native_t *port = lv_display_get_driver_data(root);
    lv_display_delete(root);
    free_port(port);
}
