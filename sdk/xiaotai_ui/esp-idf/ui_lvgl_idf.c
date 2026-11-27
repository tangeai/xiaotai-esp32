// SPDX-License-Identifier: MIT
#include "lvgl/ui_lvgl_private.h"
#include "src/display/lv_display_private.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#include <string.h>

#include "soc/soc_caps.h"
#include "ui_lvgl_ppa.h"
#include "xiaotai/ui_config.h"

/* Frame-buffer engines this build can scan out from: MIPI DSI's DPI engine
 * wherever the chip has one, the RGB LCD engine only where an RGB panel part
 * selected it (XIAOTAI_UI_RGB_SCANOUT), so other boards keep no slots. */
#if SOC_MIPI_DSI_SUPPORTED
#define UI_SCANOUT_DPI 1
#else
#define UI_SCANOUT_DPI 0
#endif
#if SOC_LCD_RGB_SUPPORTED && XIAOTAI_UI_RGB_SCANOUT
#define UI_SCANOUT_RGB 1
#else
#define UI_SCANOUT_RGB 0
#endif
#define UI_SCANOUT (UI_SCANOUT_DPI || UI_SCANOUT_RGB)

#if UI_SCANOUT
#include <stdatomic.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#if UI_SCANOUT_DPI
#include "esp_idf_version.h"
#include "esp_lcd_mipi_dsi.h"
#endif
#if UI_SCANOUT_RGB
#include "esp_lcd_panel_rgb.h"
#endif

#define SCANOUT_SLOTS 4U
/* Several frame periods of every panel driven here: by the end of the wait a
 * continuously refreshing engine has left the buffer it showed before. */
#define SCANOUT_WAIT_MS 100U

/* The ISR never refers to a UI display or heap object. A late callback can
 * only see a static slot, even when IDF is replacing the callback and ctx. */
typedef struct {
    _Atomic(esp_lcd_panel_handle_t) panel;
    StaticSemaphore_t storage;
    SemaphoreHandle_t done;
    uint8_t kind;               /* XIAOTAI_SCANOUT_* the callbacks were registered for */
} scanout_irq_t;

static scanout_irq_t scanout_irqs[SCANOUT_SLOTS];

/* Both engines report "this frame buffer can be reused safely": the engine
 * has moved on to the buffer presented last. */
static bool IRAM_ATTR scanout_signal(void *context, esp_lcd_panel_handle_t panel)
{
    scanout_irq_t *slot = context;
    if (!slot || atomic_load_explicit(&slot->panel, memory_order_acquire) != panel) return false;
    BaseType_t wake = pdFALSE;
    (void)xSemaphoreGiveFromISR(slot->done, &wake);
    return wake == pdTRUE;
}

#if UI_SCANOUT_DPI
static bool IRAM_ATTR dpi_completed(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *event, void *context)
{
    (void)event;
    return scanout_signal(context, panel);
}
#endif

#if UI_SCANOUT_RGB
static bool IRAM_ATTR rgb_completed(esp_lcd_panel_handle_t panel,
                                    const esp_lcd_rgb_panel_event_data_t *event, void *context)
{
    (void)event;
    return scanout_signal(context, panel);
}
#endif

static bool scanout_supported(uint8_t kind)
{
    return (UI_SCANOUT_DPI && kind == XIAOTAI_SCANOUT_DPI) || (UI_SCANOUT_RGB && kind == XIAOTAI_SCANOUT_RGB);
}

/* Registers the completion callback for slot, or clears it when slot is NULL. */
static xiaotai_err_t scanout_register(uint8_t kind, esp_lcd_panel_handle_t panel, scanout_irq_t *slot)
{
    switch (kind) {
#if UI_SCANOUT_DPI
    case XIAOTAI_SCANOUT_DPI: {
        esp_lcd_dpi_panel_event_callbacks_t callbacks = {0};
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0) && ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0)) || \
    ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 3)
        if (slot) callbacks.on_frame_buf_complete = dpi_completed;
#else
        if (slot) callbacks.on_refresh_done = dpi_completed;
#endif
        return esp_lcd_dpi_panel_register_event_callbacks(panel, &callbacks, slot);
    }
#endif
#if UI_SCANOUT_RGB
    case XIAOTAI_SCANOUT_RGB: {
        esp_lcd_rgb_panel_event_callbacks_t callbacks = {0};
        if (slot) callbacks.on_frame_buf_complete = rgb_completed;
        return esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, slot);
    }
#endif
    default:
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
}

static xiaotai_err_t scanout_attach(uint8_t kind, esp_lcd_panel_handle_t panel, scanout_irq_t **out)
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
    slot->kind = kind;
    atomic_store(&slot->panel, panel);
    const xiaotai_err_t err = scanout_register(kind, panel, slot);
    if (err != XIAOTAI_OK) atomic_store(&slot->panel, NULL);
    else *out = slot;
    return err;
}

static xiaotai_err_t scanout_detach(scanout_irq_t *slot)
{
    const esp_lcd_panel_handle_t panel = atomic_load(&slot->panel);
    const xiaotai_err_t err = scanout_register(slot->kind, panel, NULL);
    if (err == XIAOTAI_OK) atomic_store(&slot->panel, NULL);
    return err;
}
#endif

/* XIAOTAI_UI_PRESENT_TASK: the task that hands strips to the panel. Its stack
 * is internal: the panel driver sends each command byte from the caller's
 * stack, which DMA cannot read in external RAM (each would be copied first). */
#define PRESENT_STACK_BYTES 3072U

typedef struct {
    ui_display_t *display;
    void *pixels, *second;
    uint8_t *visited;
    uint8_t next_scanout;
    /* Both frame buffers belong to LVGL, which redraws only what changed (see ui_lvgl_port_open). */
    bool in_place;
    lv_draw_buf_t scanout[2];
#if UI_SCANOUT
    scanout_irq_t *irq;
#endif
    /* Strips drawn into two buffers: one is sent by the present task while LVGL draws the next
     * into the other. `busy` (UI task only): a strip was handed over and not yet waited for. */
    bool sending, busy, quit;
    uint8_t bpp;
    xiaotai_task_t sender;
    xiaotai_signal_t job, done;
    xiaotai_rect_t job_area;
    const void *job_pixels;
} native_t;

static void note_error(ui_display_t *display)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    ++display->frame.errors;
    xiaotai_mutex_unlock(&g_ui.lock);
}

/* The present task: one strip per job, then done. Only it calls present()
 * while a strip is handed over; the UI task waits for done before any other
 * call on the part (port_wait), so calls on the part never overlap. */
static void present_task(void *arg)
{
    native_t *port = arg;
    ui_display_t *display = port->display;
    for (;;) {
        (void)xiaotai_signal_take(&port->job, XIAOTAI_WAIT_FOREVER);
        if (port->quit) break;
        const xiaotai_rect_t area = port->job_area;
        const uint32_t pixels = (uint32_t)area.width * area.height;
        const int64_t start = xiaotai_time_us();
        const xiaotai_err_t err = display->part->present(display->part->self, &area, port->job_pixels);
        const int64_t took = xiaotai_time_us() - start;
        ui_count_flush(display, pixels, (size_t)pixels * port->bpp, took > 0 ? (uint64_t)took : 0);
        if (err != XIAOTAI_OK) note_error(display);
        xiaotai_signal_give(&port->done);
    }
    xiaotai_signal_give(&port->done);
    xiaotai_task_exit();
}

/* Until the strip handed over last has left its buffer. */
static void port_wait(native_t *port)
{
    if (!port->busy) return;
    (void)xiaotai_signal_take(&port->done, XIAOTAI_WAIT_FOREVER);
    port->busy = false;
}

/* LVGL asks before it draws into, or hands over, the buffer being sent. */
static void flush_wait(lv_display_t *root) { port_wait(lv_display_get_driver_data(root)); }

static xiaotai_err_t start_sender(native_t *port)
{
    xiaotai_err_t err = xiaotai_signal_init(&port->job);
    if (err != XIAOTAI_OK) return err;
    if ((err = xiaotai_signal_init(&port->done)) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&port->job);
        return err;
    }
    const xiaotai_task_config_t config = {
        .name = "xt_present", .entry = present_task, .arg = port, .stack_bytes = PRESENT_STACK_BYTES,
        .stack = XIAOTAI_MEM_INTERNAL, .priority = XIAOTAI_UI_TASK_PRIORITY + 1U, .core = XIAOTAI_UI_TASK_CORE,
    };
    if ((err = xiaotai_task_create(&config, &port->sender)) != XIAOTAI_OK) {
        xiaotai_signal_deinit(&port->done);
        xiaotai_signal_deinit(&port->job);
        return err;
    }
    port->sending = true;
    return XIAOTAI_OK;
}

static void stop_sender(native_t *port)
{
    if (!port->sending) return;
    port_wait(port);
    port->quit = true;
    xiaotai_signal_give(&port->job);
    (void)xiaotai_signal_take(&port->done, XIAOTAI_WAIT_FOREVER);
    xiaotai_signal_deinit(&port->done);
    xiaotai_signal_deinit(&port->job);
    port->sending = false;
}

#if UI_SCANOUT
static void scanout_fault(native_t *port, bool fault)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    port->display->frame.fault = fault;
    xiaotai_mutex_unlock(&g_ui.lock);
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
#if UI_SCANOUT
    ui_display_t *display = port->display;
    const xiaotai_err_t err = display->part->present(display->part->self, area, pixels);
    if (err != XIAOTAI_OK) return err;
    /* Discard the old frame's signal, including one from before submission.
     * A fresh frame-buffer completion means GDMA has moved onto this frame. */
    (void)xSemaphoreTake(port->irq->done, 0);
    const bool confirmed = xSemaphoreTake(port->irq->done, pdMS_TO_TICKS(SCANOUT_WAIT_MS)) == pdTRUE;
    /* An engine that signals once per presentation (RGB with a GDMA link
     * switch event) can deliver that one signal before the discard above, or
     * none at all. After the wait a refreshing engine has left the old buffer
     * anyway, so the frame settles and drawing goes on; the miss is counted
     * and reported as a fault until the next confirmed frame. */
    port->next_scanout ^= 1U;
    scanout_fault(port, !confirmed);
    if (!confirmed) note_error(display);
    return XIAOTAI_OK;
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
    if (!port->in_place && desc->render == XIAOTAI_RENDER_SCANOUT && desc->buffers == 2U)
        lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout], NULL);
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
    if (port->in_place) {
        /* LVGL drew the changed areas into the hidden buffer; the frame is shown once, after the last. */
        if (lv_display_flush_is_last(root)) {
            const xiaotai_rect_t full = {.width = desc->width, .height = desc->height};
            const uint32_t changed = (uint32_t)lv_area_get_width(area) * (uint32_t)lv_area_get_height(area);
            const int64_t began = xiaotai_time_us();
            const xiaotai_err_t shown = send_scanout(port, &full, source->data);
            const int64_t took = xiaotai_time_us() - began;
            ui_count_flush(display, changed, (size_t)changed * 2U, took > 0 ? (uint64_t)took : 0);
            if (shown != XIAOTAI_OK) note_error(display);
        }
        goto done;
    }
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
    if (port->sending) {
        /* Strips are drawn afresh each time: what was done to this one for the
         * panel needs no undoing. LVGL draws the next strip into the other
         * buffer meanwhile and waits (flush_wait) before it reuses this one. */
        port->job_area = native;
        port->job_pixels = data;
        port->busy = true;
        xiaotai_signal_give(&port->job);
        return;
    }
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

void ui_lvgl_port_settle(lv_display_t *root)
{
    native_t *port = root ? lv_display_get_driver_data(root) : NULL;
    if (!port || !port->sending) return;
    port_wait(port);
    root->flushing = 0;
    root->flushing_last = 0;
}

bool ui_lvgl_port_push(ui_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    ui_lvgl_port_settle(display->host.root);
    const xiaotai_rect_t rect = {area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area)};
    const uint32_t count = (uint32_t)rect.width * rect.height;
    const int64_t start = xiaotai_time_us();
    const xiaotai_err_t err = display->part->present(display->part->self, &rect, pixels);
    const int64_t duration = xiaotai_time_us() - start;
    ui_count_flush(display, count, (size_t)count * 2U, duration > 0 ? (uint64_t)duration : 0);
    if (err != XIAOTAI_OK) note_error(display);
    return err == XIAOTAI_OK;
}

static void free_port(native_t *port)
{
    stop_sender(port);
    if (port->in_place) ui_lvgl_ppa_detach(port->scanout, 2);
#if UI_SCANOUT
    if (port->irq) {
        const xiaotai_err_t err = scanout_detach(port->irq);
        if (err != XIAOTAI_OK) port->display->native_cleanup_error = err;
    }
#endif
    xiaotai_mem_free(port->visited);
    xiaotai_mem_free(port->second);
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
#if UI_SCANOUT
    if (scanout && desc->buffers == 2U && !scanout_supported(display->handles.scanout_kind))
        return XIAOTAI_ERR_NOT_SUPPORTED;
#else
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
        port->bpp = (uint8_t)bpp;
        /* Without the second buffer or the task, the UI task sends each strip itself. */
        if (XIAOTAI_UI_PRESENT_TASK && partial) {
            port->second = xiaotai_mem_aligned(memory, align, (size_t)bytes);
            if (port->second && start_sender(port) != XIAOTAI_OK) {
                xiaotai_mem_free(port->second);
                port->second = NULL;
            }
        }
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
    /* A double-buffered panel that scans out its own frame buffers (RGB or MIPI DPI) is drawn in
     * place: LVGL keeps both frame buffers, redraws the areas that changed and copies what the
     * previous frame changed, so a small animation no longer costs a whole frame of external memory
     * traffic (a whole 1024x600 frame: about 70 ms on the JC1060F570C's RGB panel, 65 ms on the
     * WT99's DPI panel). */
    port->in_place = scanout && desc->buffers == 2U && (display->handles.scanout_kind == XIAOTAI_SCANOUT_RGB ||
                                                        display->handles.scanout_kind == XIAOTAI_SCANOUT_DPI);
    lv_display_set_render_mode(root, partial ? LV_DISPLAY_RENDER_MODE_PARTIAL :
                               port->in_place ? LV_DISPLAY_RENDER_MODE_DIRECT : LV_DISPLAY_RENDER_MODE_FULL);
    if (scanout) lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout], NULL);
    else lv_display_set_buffers_with_stride(root, port->pixels, port->sending ? port->second : NULL, (uint32_t)bytes,
                      stride, partial ? LV_DISPLAY_RENDER_MODE_PARTIAL : LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_driver_data(root, port);
    lv_display_set_flush_cb(root, flush);
    if (port->sending) lv_display_set_flush_wait_cb(root, flush_wait);
    lv_timer_pause(lv_display_get_refr_timer(root));
#if UI_SCANOUT
    if (scanout && desc->buffers == 2U) {
        err = scanout_attach(display->handles.scanout_kind, display->handles.panel, &port->irq);
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
        lv_display_set_draw_buffers(root, &port->scanout[port->next_scanout],
                                    port->in_place ? &port->scanout[port->next_scanout ^ 1U] : NULL);
        /* Frame buffers drawn in place sit in external memory: where the chip has a pixel
         * accelerator it takes the block moves. Without one, or when it declines these
         * buffers, the software renderer keeps doing all of it. */
        if (port->in_place) (void)ui_lvgl_ppa_attach(port->scanout, 2);
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
    ui_lvgl_port_settle(root);
    lv_display_delete(root);
    free_port(port);
}

void ui_lvgl_port_reset(void) { ui_lvgl_ppa_reset(); }

void ui_lvgl_port_report(char *words, size_t capacity) { ui_lvgl_ppa_report(words, capacity); }

xiaotai_err_t ui_lvgl_port_accelerate(bool on) { return ui_lvgl_ppa_enable(on); }
