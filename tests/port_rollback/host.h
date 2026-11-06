#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define LVGL_PORT_PPA 0
#define CONFIG_LCD_RGB_ISR_IRAM_SAFE 0
#define CONFIG_LV_DRAW_BUF_ALIGN 4
#define SOC_PSRAM_DMA_CAPABLE 1
#define SOC_LCDCAM_RGB_LCD_SUPPORTED 0
#define SOC_MIPI_DSI_SUPPORTED 1
#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))
#define ESP_IDF_VERSION 60002
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_NOT_SUPPORTED 3
#define ESP_RETURN_ON_FALSE(ok, value, ...) do { if (!(ok)) return (value); } while (0)
#define ESP_GOTO_ON_FALSE(ok, value, label, ...) do { if (!(ok)) { ret = (value); goto label; } } while (0)
#define ESP_GOTO_ON_ERROR(value, label, ...) do { ret = (value); if (ret) goto label; } while (0)
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_DEFAULT 4
#define MALLOC_CAP_INTERNAL 8
#define MALLOC_CAP_8BIT 16
#define LV_COLOR_FORMAT_RGB565 1
#define LV_COLOR_FORMAT_RGB565_SWAPPED 2
#define LV_COLOR_FORMAT_RGB888 3
#define LV_COLOR_FORMAT_XRGB8888 4
#define LV_COLOR_FORMAT_ARGB8888 5
#define LV_COLOR_FORMAT_I1 6
#define LV_DISPLAY_ROTATION_0 0
#define LV_DISPLAY_RENDER_MODE_FULL 1
#define LV_DISPLAY_RENDER_MODE_DIRECT 2
#define LV_DISPLAY_RENDER_MODE_PARTIAL 3
#define LVGL_PORT_DISP_TYPE_DSI 1
#define LV_EVENT_RESOLUTION_CHANGED 1
#define LV_EVENT_INVALIDATE_AREA 2
#define LV_EVENT_REFR_REQUEST 3
#define lvgl_port_flush_callback 0
#define lvgl_port_disp_size_update_callback 0
#define lvgl_port_display_invalidate_callback 0

typedef int esp_err_t, lvgl_port_disp_type_t, lv_display_rotation_t, lv_color_format_t;
typedef uint16_t lv_color_t;
typedef void *esp_lcd_panel_io_handle_t, *esp_lcd_panel_handle_t, *SemaphoreHandle_t;
typedef void (*lvgl_port_rounder_cb)(void);
typedef lvgl_port_rounder_cb lvgl_port_rounder_cb_t;
typedef struct { bool swap_xy, mirror_x, mirror_y; } lvgl_port_rotation_cfg_t;
typedef struct { void *data; } lv_display_t;
typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle, control_handle;
    lvgl_port_rotation_cfg_t rotation;
    uint32_t buffer_size, hres, vres;
    int color_format;
    bool double_buffer, monochrome;
    lvgl_port_rounder_cb_t rounder_cb;
    struct { bool swap_bytes, sw_rotate, buff_dma, buff_spiram, direct_mode, full_refresh; } flags;
} lvgl_port_display_cfg_t;
typedef struct { int disp_type; bool avoid_tearing; } lvgl_port_disp_priv_cfg_t;

static int alloc_calls, fail_at, live, frame_failure;
static void *allocations[16];
static uint16_t panel_buffers[2][64];
static void *host_alloc(size_t size)
{
    if (++alloc_calls == fail_at) return NULL;
    void *p = calloc(1, size);
    assert(p && live < 16);
    allocations[live++] = p;
    return p;
}
static void host_free(void *p)
{
    if (!p) return;
    int i;
    for (i = 0; i < live && allocations[i] != p; ++i) {}
    assert(i < live); /* Includes rejecting borrowed panel framebuffers. */
    allocations[i] = allocations[--live];
    free(p);
}
#define malloc(size) host_alloc(size)
#define free(p) host_free(p)
#define heap_caps_malloc(size, caps) host_alloc(size)
#define heap_caps_aligned_alloc(align, size, caps) ((void)(align), (void)(caps), host_alloc(size))
#define xSemaphoreCreateCounting(max, initial) host_alloc(1)
#define vSemaphoreDelete(p) host_free(p)
static int esp_lcd_dpi_panel_get_frame_buffer(void *panel, int n, void **a, void **b)
{
    if (frame_failure) return ESP_ERR_INVALID_ARG;
    assert(panel && n == 2);
    *a = panel_buffers[0]; *b = panel_buffers[1]; return ESP_OK;
}
static int lv_color_format_get_size(int format) { return format == LV_COLOR_FORMAT_I1 ? 1 : 2; }
static lv_display_t *lv_display_create(int w, int h) { assert(w && h); return host_alloc(sizeof(lv_display_t)); }
static void lv_display_delete(lv_display_t *d) { assert(d); host_free(d); }
#define lv_disp_remove(d) lv_display_delete(d)
#define lv_display_set_color_format(d, f) do { assert(d); (void)(f); } while (0)
#define lv_display_set_buffers(d, a, b, s, mode) do { assert((d) && (a) && (s) > 0); (void)(b); (void)(mode); } while (0)
#define lv_display_set_flush_cb(d, cb) assert(d)
#define lv_display_add_event_cb(d, cb, event, ctx) do { assert((d) && (ctx)); } while (0)
static void lv_display_set_driver_data(lv_display_t *d, void *data) { assert(d); d->data = data; }
static void *lv_display_get_driver_data(lv_display_t *d) { assert(d); return d->data; }
#define lvgl_port_lock(timeout) ((void)(timeout))
#define lvgl_port_unlock() ((void)0)
