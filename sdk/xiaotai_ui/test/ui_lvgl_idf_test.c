// SPDX-License-Identifier: MIT
// Replaces legacy: tests/test_cores3_board_display.py
// Replaces legacy: tests/test_s3_panel_mapping.py
// Replaces legacy: tests/regression/test_p4_display_port.py
// Replaces legacy: tests/test_dsi_contract.py
#include "lvgl/ui_lvgl_private.h"
#include "src/display/lv_display_private.h"
#include "xiaotai/port/mem.h"
#include "soc/soc_caps.h"

#include <assert.h>
#include <string.h>

static unsigned presents;
static xiaotai_err_t present_error;
static xiaotai_mem_kind_t last_aligned;
static size_t last_align;
#if !SOC_MIPI_DSI_SUPPORTED
static size_t last_size;
#endif
static xiaotai_display_t part;
static ui_display_t display;
static xiaotai_rect_t presented_area;
static uint8_t presented_pixels[256];

void *__real_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size);
void *__wrap_xiaotai_mem_aligned(xiaotai_mem_kind_t kind, size_t alignment, size_t size)
{
    last_aligned = kind;
    last_align = alignment;
#if !SOC_MIPI_DSI_SUPPORTED
    last_size = size;
#endif
    return __real_xiaotai_mem_aligned(kind, alignment, size);
}

#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#include "freertos/semphr.h"
#include "xiaotai_mem_host.h"

static struct test_idf_panel { int unused; } panel;
static fixture_dpi_callback_t completed, late;
static void *context, *late_context;
static const void *front, *requested;
static bool can_complete, switch_in_present;
static unsigned attached, detached, timed_waits, drained;
static xiaotai_err_t attach_error;
static bool fail_display_create, fail_scanout_sem;

lv_display_t *__real_lv_display_create(int32_t width, int32_t height);
lv_display_t *__wrap_lv_display_create(int32_t width, int32_t height)
{
    return fail_display_create ? NULL : __real_lv_display_create(width, height);
}

int esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t dev,
    const esp_lcd_dpi_panel_event_callbacks_t *callbacks, void *ctx)
{
    assert(dev == &panel && !callbacks->on_color_trans_done);
#if defined(UI_TEST_IDF_504) || defined(UI_TEST_IDF_602)
    const fixture_dpi_callback_t next = callbacks->on_refresh_done;
#else
    const fixture_dpi_callback_t next = callbacks->on_frame_buf_complete;
#endif
    if (next && attach_error != XIAOTAI_OK) return attach_error;
    completed = next;
    context = ctx;
    if (completed) {
        assert(ctx);
        late = completed;
        late_context = context;
        ++attached;
    } else {
        assert(!ctx);
        ++detached;
    }
    return XIAOTAI_OK;
}

static void refresh_complete(void)
{
    assert(completed);
    front = requested;
    (void)completed(&panel, NULL, context);
}

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage)
{
    if (fail_scanout_sem) return NULL;
    *storage = (StaticSemaphore_t){0};
    return storage;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{
    sem->given = true;
    *woken = pdTRUE;
    return pdTRUE;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    if (ticks) {
        assert(ticks == 100 && !sem->given); /* submission's stale signal removed first */
        ++timed_waits;
        if (can_complete) refresh_complete();
    } else if (sem->given) ++drained;
    const bool given = sem->given;
    sem->given = false;
    return given;
}
#endif

static xiaotai_err_t present(void *state, const xiaotai_rect_t *area, const void *pixels)
{
    assert(state == &part);
    ++presents;
    presented_area = *area;
    const size_t count = (size_t)area->width * area->height * 2U;
    assert(count <= sizeof(presented_pixels));
    memcpy(presented_pixels, pixels, count);
#if SOC_MIPI_DSI_SUPPORTED
    if (part.desc.render == XIAOTAI_RENDER_SCANOUT) {
        assert(pixels == display.handles.framebuffers[0] || pixels == display.handles.framebuffers[1]);
        /* An interrupt for the earlier front buffer racing submission is not
         * permission to start drawing into it. */
        (void)completed(&panel, NULL, context);
        if (present_error == XIAOTAI_OK) requested = pixels;
        /* An engine that signals once per presentation may already have
         * switched, and signalled, before the caller looks for the signal. */
        if (switch_in_present) refresh_complete();
    }
#endif
    return present_error;
}

static void fixture(xiaotai_display_desc_t desc)
{
    presents = 0;
    present_error = XIAOTAI_OK;
    part = (xiaotai_display_t){.desc = desc, .present = present, .self = &part};
    display = (ui_display_t){.part = &part, .power.awake = true};
}

static void owned_buffers(void)
{
    lv_init();
    fixture((xiaotai_display_desc_t){.width = 3, .height = 2, .render = XIAOTAI_RENDER_FULL,
        .rotation = 3, .buffer_align = 128, .flags = XIAOTAI_DISPLAY_SWAP_BYTES | XIAOTAI_DISPLAY_DMA_BUFFER});
    lv_display_t *root;
    assert(ui_lvgl_port_open(&display, &root) == XIAOTAI_OK);
    assert(last_aligned == XIAOTAI_MEM_BULK && last_align == 128);
    uint8_t *data = lv_display_get_buf_active(root)->data;
    const uint8_t logical[] = {1,0,2,0,3,0,4,0,5,0,6,0}, native[] = {0,3,0,6,0,2,0,5,0,1,0,4};
    memcpy(data, logical, sizeof(logical));
    root->flush_cb(root, &(lv_area_t){0, 0, 2, 1}, data);
    assert(presents == 1 && presented_area.width == 2 && presented_area.height == 3);
    assert(!memcmp(presented_pixels, native, sizeof(native)) && !memcmp(data, logical, sizeof(logical)));
    ui_lvgl_port_close(&display, root);

    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_PARTIAL,
        .transfer_pixels = 16, .buffer_align = 16, .flags = XIAOTAI_DISPLAY_DMA_BUFFER});
    assert(ui_lvgl_port_open(&display, &root) == XIAOTAI_OK);
    assert(last_aligned == XIAOTAI_MEM_DMA && last_align == 16);
    lv_draw_buf_t *buffer = lv_display_get_buf_active(root);
    assert(buffer->data_size >= 32);
    buffer->header.stride = 12;  /* dirty area with padding (native requires packed rows) */
    data = buffer->data;
    memset(data, 0x42, 8);
    memset(data + 12, 0x18, 8);
    present_error = XIAOTAI_ERR_TIMEOUT;
    root->flush_cb(root, &(lv_area_t){2, 1, 5, 2}, data);
    assert(presents == 1 && display.frame.errors == 1 && !root->flushing);
    assert(presented_area.x == 2 && presented_area.y == 1 && presented_area.width == 4 && presented_area.height == 2);
    for (size_t i = 0; i < 8; ++i) {
        assert(presented_pixels[i] == 0x42 && presented_pixels[8U + i] == 0x18);
        assert(data[i] == 0x42 && data[12U + i] == 0x18);
    }
    ui_lvgl_port_close(&display, root);
    lv_deinit();
}

#if XIAOTAI_UI_PRESENT_TASK
/* Strips leave from the present task while LVGL draws into the other buffer. */
static void strips_sent_beside(void)
{
    lv_init();
    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_PARTIAL,
        .transfer_pixels = 16, .flags = XIAOTAI_DISPLAY_SWAP_BYTES});
    lv_display_t *root;
    assert(ui_lvgl_port_open(&display, &root) == XIAOTAI_OK);
    display.host.root = root;
    assert(root->buf_1 && root->buf_2 && root->buf_1->data != root->buf_2->data && root->flush_wait_cb);
    assert(root->render_mode == LV_DISPLAY_RENDER_MODE_PARTIAL);
    const uint8_t sample[] = {0x12, 0x34, 0x56, 0x78}, swapped[] = {0x34, 0x12, 0x78, 0x56};
    uint8_t *first = root->buf_1->data, *second = root->buf_2->data;
    memcpy(first, sample, sizeof(sample));
    /* As LVGL does: flushing set before the strip is handed over, waited for before the buffer is reused. */
    root->flushing = 1;
    root->flush_cb(root, &(lv_area_t){2, 1, 3, 1}, first);
    assert(root->flushing);
    root->flush_wait_cb(root);
    assert(presents == 1 && !memcmp(presented_pixels, swapped, sizeof(swapped)));
    assert(presented_area.x == 2 && presented_area.y == 1 && presented_area.width == 2 && presented_area.height == 1);
    root->flushing = 0;
    ui_lvgl_port_settle(root);   /* nothing in flight */
    assert(presents == 1);

    /* After a refresh the last strip may still be on its way: settling waits for it. */
    memcpy(second, sample, sizeof(sample));
    present_error = XIAOTAI_ERR_TIMEOUT;
    root->flushing = 1;
    root->flush_cb(root, &(lv_area_t){0, 2, 1, 2}, second);
    ui_lvgl_port_settle(root);
    assert(!root->flushing && !root->flushing_last && presents == 2 && display.frame.errors == 1);
    assert(display.flushes == 2 && display.flush_pixels == 4 && display.flush_bytes == 8);

    /* A direct paint goes out only after the strip before it. */
    present_error = XIAOTAI_OK;
    root->flushing = 1;
    root->flush_cb(root, &(lv_area_t){0, 3, 1, 3}, first);
    uint8_t direct[4] = {1, 2, 3, 4};
    assert(ui_lvgl_port_push(&display, &(lv_area_t){4, 4, 5, 4}, direct));
    assert(presents == 4 && !root->flushing && presented_area.x == 4 && !memcmp(presented_pixels, direct, 4));

    /* Closing with a strip on its way waits for it, then ends the task. */
    root->flushing = 1;
    root->flush_cb(root, &(lv_area_t){0, 4, 1, 4}, second);
    ui_lvgl_port_close(&display, root);
    assert(presents == 5);
    lv_deinit();
}
#endif

#if !SOC_MIPI_DSI_SUPPORTED
static xiaotai_display_handles_t opened_handles;
static xiaotai_err_t open_error;

static xiaotai_err_t board_open(void *ctx, xiaotai_display_handles_t *out)
{
    assert(ctx == &part);
    *out = opened_handles;
    return open_error;
}

static xiaotai_err_t start_theme(const xiaotai_ui_host_t *host, void **state)
{
    *state = xiaotai_ui_host_root(host);
    return *state ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_STATE;
}

static void s3_panel_mapping(void)
{
    static char io_handle, panel_handle, touch_handle;
    static const xiaotai_theme_t theme = {.backend = XIAOTAI_UI_BACKEND_LVGL, .start = start_theme};
    const uint8_t sample[] = {0x12, 0x34, 0x56, 0x78}, swapped[] = {0x34, 0x12, 0x78, 0x56};
    for (unsigned partial = 0; partial < 2; ++partial) {
        for (unsigned alternate = 0; alternate < 2; ++alternate) {
            const uint16_t tile = alternate ? 1600U : 3200U;
            fixture((xiaotai_display_desc_t){.width = 320, .height = 240, .pixel = XIAOTAI_PIXEL_RGB565,
                .touch = true, .render = partial ? XIAOTAI_RENDER_PARTIAL : XIAOTAI_RENDER_FULL,
                .transfer_pixels = tile, .flags = (partial ? XIAOTAI_DISPLAY_DMA_BUFFER : 0) |
                                                 (alternate ? 0 : XIAOTAI_DISPLAY_SWAP_BYTES)});
            part.open = board_open;
            display.theme = &theme;
            opened_handles = (xiaotai_display_handles_t){.panel_io = &io_handle, .panel = &panel_handle,
                .touch = &touch_handle, .swap_xy = partial == alternate, .mirror_x = partial == alternate,
                .mirror_y = alternate != 0};
            open_error = XIAOTAI_OK;
            assert(ui_display_open(&display) == XIAOTAI_OK);
            lv_display_t *root = display.host.root;
            assert(root && display.theme_state == root);
            assert(lv_display_get_horizontal_resolution(root) == 320 && lv_display_get_vertical_resolution(root) == 240);
            assert(lv_display_get_color_format(root) == LV_COLOR_FORMAT_RGB565 && !root->buf_2);
            assert(root->render_mode == (partial ? LV_DISPLAY_RENDER_MODE_PARTIAL : LV_DISPLAY_RENDER_MODE_FULL));
            assert(last_size == 2U * (partial ? tile : 320U * 240U));
            assert(last_aligned == (partial ? XIAOTAI_MEM_DMA : XIAOTAI_MEM_BULK));
            uint8_t *pixels = lv_display_get_buf_active(root)->data;
            memcpy(pixels, sample, sizeof(sample));
            root->flush_cb(root, &(lv_area_t){0, 0, 1, 0}, pixels);
            assert(presents == 1 && !memcmp(presented_pixels, alternate ? sample : swapped, sizeof(sample)));

            assert(display.handles.panel_io == opened_handles.panel_io && display.handles.panel == opened_handles.panel);
            assert(display.handles.touch == opened_handles.touch && display.handles.swap_xy == opened_handles.swap_xy);
            assert(display.handles.mirror_x == opened_handles.mirror_x && display.handles.mirror_y == opened_handles.mirror_y);
            ui_display_stop(&display);

            opened_handles = (xiaotai_display_handles_t){0};
            open_error = XIAOTAI_ERR_TIMEOUT;
            assert(ui_display_open(&display) == XIAOTAI_ERR_TIMEOUT && display.host.root == NULL && presents == 1);
            assert(display.handles.panel_io == &io_handle && display.handles.panel == &panel_handle);
            assert(display.handles.touch == &touch_handle && display.handles.swap_xy == (partial == alternate));
            assert(display.handles.mirror_x == (partial == alternate) && display.handles.mirror_y == (alternate != 0));
        }
    }
    ui_lvgl_shutdown();
}
#endif

#if SOC_MIPI_DSI_SUPPORTED
static void assert_open_rolled_back(void)
{
    assert(!display.host.root && !display.lvgl && !display.drawing && !lv_is_initialized());
    assert(!completed && !context && attached == detached);
    assert(xiaotai_mem_host_outstanding() == 0);
}

static void allocation_sweep(const void *borrowed, const void *before, size_t bytes)
{
    xiaotai_mem_host_clear();
    assert(ui_lvgl_open(&display) == XIAOTAI_OK);
    const uint32_t allocations = xiaotai_mem_host_allocations();
    assert(allocations >= 3); /* backend, port, and painter table */
    ui_lvgl_close(&display);
    assert_open_rolled_back();
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        for (uint32_t failure = 1; failure <= allocations + 1; ++failure) {
            xiaotai_mem_host_clear();
            fail_display_create = failure > allocations;
            if (!fail_display_create) xiaotai_mem_host_fail_nth(failure);
            assert(ui_lvgl_open(&display) == XIAOTAI_ERR_NO_MEM);
            fail_display_create = false;
            assert(!xiaotai_mem_host_fail_pending());
            assert_open_rolled_back();
            if (borrowed) assert(!memcmp(borrowed, before, bytes));
            assert(ui_lvgl_open(&display) == XIAOTAI_OK);
            ui_lvgl_close(&display);
            assert_open_rolled_back();
        }
    }
}

static void scanout_rollback(void)
{
    static _Alignas(64) uint8_t fb[2][96];
    memset(fb[0], 0x51, sizeof(fb[0]));
    memset(fb[1], 0x2a, sizeof(fb[1]));
    uint8_t before[sizeof(fb)];
    memcpy(before, fb, sizeof(before));
    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_SCANOUT, .buffers = 2});
    display.handles = (xiaotai_display_handles_t){.panel = &panel, .framebuffers = {fb[0], fb[1]},
                                                .scanout_kind = XIAOTAI_SCANOUT_DPI};
    front = fb[1];
    can_complete = true;

    xiaotai_mem_host_clear();
    const unsigned original_attached = attached, original_detached = detached;
    attach_error = XIAOTAI_ERR_INVALID_ARG;
    assert(ui_lvgl_open(&display) == XIAOTAI_ERR_INVALID_ARG);
    attach_error = XIAOTAI_OK;
    assert_open_rolled_back();
    assert(!presents && attached == original_attached && detached == original_detached && front == fb[1]);
    assert(!memcmp(fb, before, sizeof(fb)));

    present_error = XIAOTAI_ERR_TIMEOUT; /* a failed submission rolls the open back */
    assert(ui_lvgl_open(&display) == XIAOTAI_ERR_TIMEOUT);
    present_error = XIAOTAI_OK;
    assert(presents == 1 && !display.frame.fault && !display.frame.errors && front == fb[1]);
    assert_open_rolled_back();
    assert(attached == original_attached + 1 && detached == original_detached + 1);
    assert(!late(&panel, NULL, late_context) && !memcmp(fb, before, sizeof(fb)));

    /* No completion signal at all, as after a CPU-only restart: the display
     * still opens with the miss reported, and a confirmed frame clears it. */
    can_complete = false;
    assert(ui_lvgl_open(&display) == XIAOTAI_OK);
    assert(presents == 2 && display.frame.fault && display.frame.errors == 1);
    assert(lv_display_get_buf_active(display.host.root)->data == fb[1]);
    ui_lvgl_close(&display);
    assert_open_rolled_back();

    can_complete = true;
    assert(ui_lvgl_open(&display) == XIAOTAI_OK);
    assert(!display.frame.fault && front == fb[0] && lv_display_get_buf_active(display.host.root)->data == fb[1]);
    ui_lvgl_close(&display);
    assert_open_rolled_back();

    allocation_sweep(fb, before, sizeof(fb));
    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_FULL,
        .rotation = 3, .flags = XIAOTAI_DISPLAY_SWAP_BYTES});
    allocation_sweep(NULL, NULL, 0);
    ui_lvgl_shutdown();
}

static void scanout_path(void)
{
    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_SCANOUT, .buffers = 2});
    static _Alignas(64) uint8_t fb[2][96];
    display.handles = (xiaotai_display_handles_t){.panel = &panel, .framebuffers = {fb[0], fb[1]}};
    lv_display_t *root;
    assert(ui_lvgl_port_open(&display, &root) == XIAOTAI_ERR_NOT_SUPPORTED && !root && !attached && !presents);
    display.handles.scanout_kind = XIAOTAI_SCANOUT_DPI;
    front = fb[1]; /* BSP can already be open and displaying the second buffer. */
    xiaotai_mem_host_clear();
    fail_scanout_sem = true;
    assert(ui_lvgl_open(&display) == XIAOTAI_ERR_NO_MEM);
    fail_scanout_sem = false;
    assert_open_rolled_back();
    assert(!presents && !attached && front == fb[1]);
    can_complete = true;
    assert(ui_lvgl_open(&display) == XIAOTAI_OK);
    root = display.host.root;
    assert(front == fb[0] && presents == 1 && lv_display_get_buf_active(root)->data == fb[1]);
    ui_lvgl_enter(&display);
    lv_obj_t *screen = lv_display_get_screen_active(root);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xff0000), 0);
    ui_lvgl_step(&display, 0);
    assert(front == fb[1] && presents == 2 && timed_waits == 2 && drained == 2);
    uint8_t before[sizeof(fb)];
    memcpy(before, fb, sizeof(before));
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x00ff00), 0);
    xiaotai_ui_capture_t shot;
    assert(ui_lvgl_capture(&display, &shot) == XIAOTAI_OK);
    assert(shot.pixels[0] == 0xe0 && shot.pixels[1] == 0x07 && front == fb[1] && presents == 2);
    assert(!memcmp(fb, before, sizeof(before)));
    xiaotai_ui_capture_free(&shot);

    /* The only signal for this frame lands before the stale-signal discard and
     * none follows: the frame is reported, and drawing still goes on. */
    can_complete = false;
    switch_in_present = true;
    ui_lvgl_step(&display, 1);
    switch_in_present = false;
    assert(front == fb[0] && presents == 3 && display.frame.errors == 1 && display.frame.fault);
    assert(ui_lvgl_port_ready(root) && lv_display_get_buf_active(root)->data == fb[1]);
    can_complete = true;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000ff), 0);
    ui_lvgl_step(&display, 2);
    assert(front == fb[1] && presents == 4 && display.frame.errors == 1 && !display.frame.fault);
    assert(ui_lvgl_port_ready(root) && lv_display_get_buf_active(root)->data == fb[0]);
    ui_lvgl_leave(&display);
    ui_lvgl_close(&display);
    ui_lvgl_shutdown();
    assert(attached == 1 && detached == 1 && !late(&panel, NULL, late_context));
}
#endif

int main(void)
{
    assert(xiaotai_mutex_init(&g_ui.lock) == XIAOTAI_OK);
#if XIAOTAI_UI_PRESENT_TASK
    /* The UI task's own sending is covered by the other builds of this test. */
    (void)owned_buffers;
    (void)s3_panel_mapping;
    strips_sent_beside();
#else
    owned_buffers();
#endif
#if XIAOTAI_UI_PRESENT_TASK
#elif SOC_MIPI_DSI_SUPPORTED
    scanout_path();
    scanout_rollback();
#else
    s3_panel_mapping();
    fixture((xiaotai_display_desc_t){.width = 8, .height = 6, .render = XIAOTAI_RENDER_SCANOUT, .buffers = 2});
    display.handles = (xiaotai_display_handles_t){.panel = &part, .framebuffers = {&part, &display},
                                                .scanout_kind = XIAOTAI_SCANOUT_DPI};
    lv_display_t *root;
    assert(ui_lvgl_port_open(&display, &root) == XIAOTAI_ERR_NOT_SUPPORTED && !root);
#endif
    xiaotai_mutex_deinit(&g_ui.lock);
    return 0;
}
