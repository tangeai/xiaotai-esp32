// SPDX-License-Identifier: MIT
#include "ui_lvgl_private.h"
#include "ui_lvgl_draw.h"
#include "xiaotai/port/mem.h"
#include "src/display/lv_display_private.h"

#include <limits.h>
#include <string.h>

typedef struct {
    lv_display_t *root;
    lv_indev_t *pointer;
    lv_obj_t *region;
    xiaotai_ui_video_source_t source;
    void (*release)(void);
    uint32_t generation, due;
    bool polled, placed;
    lv_area_t picture;
#if XIAOTAI_UI_CAPTURE
    xiaotai_ui_capture_t *capture;
#endif
} ui_lvgl_t;

static xiaotai_mutex_t lvgl_lock;
static bool mutex_ready;
static unsigned open_displays;

static xiaotai_err_t ensure_mutex(void)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    const xiaotai_err_t err = mutex_ready ? XIAOTAI_OK : xiaotai_mutex_init(&lvgl_lock);
    if (err == XIAOTAI_OK) mutex_ready = true;
    xiaotai_mutex_unlock(&g_ui.lock);
    return err;
}

static void enter(void) { (void)xiaotai_mutex_lock(&lvgl_lock, XIAOTAI_WAIT_FOREVER); }
static void leave(void) { xiaotai_mutex_unlock(&lvgl_lock); }

void ui_lvgl_enter(const ui_display_t *display)
{
    if (!display->lvgl) return;
    enter();
    lv_display_set_default(display->host.root);
}
void ui_lvgl_leave(const ui_display_t *display) { if (display->lvgl) leave(); }

void ui_lvgl_shutdown(void)
{
    /* Called after all display tasks have joined. */
    if (mutex_ready) xiaotai_mutex_deinit(&lvgl_lock);
    mutex_ready = false;
}

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    const ui_display_t *display = lv_indev_get_user_data(indev);
    data->point.x = display->pointer.point.x;
    data->point.y = display->pointer.point.y;
    data->state = display->pointer.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void even_columns(lv_event_t *event)
{
    lv_area_t *area = lv_event_get_param(event);
    if (area) {
        area->x1 &= ~1;
        area->x2 |= 1;
    }
}

xiaotai_err_t ui_lvgl_open(ui_display_t *display)
{
    const xiaotai_display_desc_t *desc = &display->part->desc;
    if ((desc->flags & XIAOTAI_DISPLAY_EVEN_AREAS) && (desc->width & 1U))
        return XIAOTAI_ERR_INVALID_SIZE;
    xiaotai_err_t err = ensure_mutex();
    if (err != XIAOTAI_OK) return err;
    ui_lvgl_t *backend = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*backend));
    if (!backend) return XIAOTAI_ERR_NO_MEM;

    enter();
    if (!open_displays) {
        lv_init();
        lv_tick_set_cb(ui_now_ms);
        err = ui_lvgl_draw_start();
        if (err != XIAOTAI_OK) goto fail;
    }
    err = ui_lvgl_port_open(display, &backend->root);
    if (err != XIAOTAI_OK) goto fail;
    display->host.root = backend->root;
    display->lvgl = backend;
    err = ui_lvgl_draw_init(display);
    if (err != XIAOTAI_OK) goto fail;
    if (display->part->desc.flags & XIAOTAI_DISPLAY_EVEN_AREAS)
        lv_display_add_event_cb(backend->root, even_columns, LV_EVENT_INVALIDATE_AREA, NULL);
    if (display->part->desc.touch) {
        backend->pointer = lv_indev_create();
        if (!backend->pointer) {
            err = XIAOTAI_ERR_NO_MEM;
            goto fail;
        }
        lv_indev_set_type(backend->pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(backend->pointer, backend->root);
        lv_indev_set_mode(backend->pointer, LV_INDEV_MODE_EVENT);
        lv_indev_set_user_data(backend->pointer, display);
        lv_indev_set_read_cb(backend->pointer, read_pointer);
    }
    ++open_displays;
    leave();
    return XIAOTAI_OK;
fail:
    if (display->drawing) ui_lvgl_draw_drop(display);
    if (backend->root) ui_lvgl_port_close(display, backend->root);
    display->host.root = NULL;
    display->lvgl = NULL;
    if (!open_displays) {
        lv_deinit();
        ui_lvgl_draw_reset();
    }
    leave();
    xiaotai_mem_free(backend);
    return err;
}

void ui_lvgl_close(ui_display_t *display)
{
    if (!display->lvgl) return;
    enter();
    ui_lvgl_t *backend = display->lvgl;
    if (backend->pointer) lv_indev_delete(backend->pointer);
    display->closed_video_release = backend->release;
    ui_lvgl_draw_drop(display);
    ui_lvgl_port_close(display, backend->root);
    display->host.root = NULL;
    display->lvgl = NULL;
    xiaotai_mem_free(backend);
    if (!--open_displays) {
        lv_deinit();
        ui_lvgl_draw_reset();
    }
    leave();
}

static bool video_scene(const ui_display_t *display)
{
    const xiaotai_ui_scene_t *scene = &display->scene;
    return (scene->page.present && (scene->page.view.flags & XIAOTAI_VIEW_VIDEO)) ||
           (scene->overlay.present && (scene->overlay.view.flags & XIAOTAI_VIEW_VIDEO));
}

static bool area_of(const lv_obj_t *widget, xiaotai_ui_rect_t *area)
{
    if (!widget || lv_obj_has_flag(widget, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t bounds;
    lv_obj_get_coords(widget, &bounds);
    if (bounds.x1 < 0 || bounds.y1 < 0 || bounds.x2 < bounds.x1 || bounds.y2 < bounds.y1 ||
        bounds.x2 >= UINT16_MAX || bounds.y2 >= UINT16_MAX) return false;
    *area = (xiaotai_ui_rect_t){.x = (uint16_t)bounds.x1, .y = (uint16_t)bounds.y1,
                               .width = (uint16_t)lv_area_get_width(&bounds),
                               .height = (uint16_t)lv_area_get_height(&bounds)};
    return true;
}

static bool video_paint(void *ctx, const xiaotai_ui_surface_t *surface, int16_t x, int16_t y)
{
    (void)x;
    (void)y;
    const ui_display_t *display = ctx;
    const ui_lvgl_t *backend = display->lvgl;
    xiaotai_ui_rect_t area;
    return backend->source.paint && video_scene(display) && area_of(backend->region, &area) &&
           backend->source.paint(backend->source.ctx, surface, area.x, area.y, area.width, area.height);
}

static void deleted_region(lv_event_t *event)
{
    ui_lvgl_t *backend = lv_event_get_user_data(event);
    if (backend && lv_event_get_target_obj(event) == backend->region) {
        backend->region = NULL;
        backend->polled = backend->placed = false;
    }
}

static void video_not_opaque(lv_event_t *event)
{
    /* Draw through the old picture rectangle even when the new image
     * covers less of it. The presenter writes only the pinned picture. */
    lv_event_set_cover_res(event, LV_COVER_RES_NOT_COVER);
}

xiaotai_err_t xiaotai_ui_host_video_region(const xiaotai_ui_host_t *host, void *widget)
{
    ui_display_t *display = host ? ui_display_at(host->index) : NULL;
    if (!display || host != &display->host) return XIAOTAI_ERR_INVALID_ARG;
    ui_lvgl_t *backend = display->lvgl;
    if (!backend) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (widget == backend->region) return XIAOTAI_OK;
    if (widget) {
        const xiaotai_err_t err = xiaotai_ui_host_painter(host, widget, video_paint, display);
        if (err != XIAOTAI_OK) return err;
        if (!lv_obj_add_event_cb(widget, deleted_region, LV_EVENT_DELETE, backend)) {
            (void)xiaotai_ui_host_painter(host, widget, NULL, NULL);
            return XIAOTAI_ERR_NO_MEM;
        }
        if (!lv_obj_add_event_cb(widget, video_not_opaque, LV_EVENT_COVER_CHECK, backend)) {
            lv_obj_remove_event_cb_with_user_data(widget, deleted_region, backend);
            (void)xiaotai_ui_host_painter(host, widget, NULL, NULL);
            return XIAOTAI_ERR_NO_MEM;
        }
    }
    if (backend->region) {
        lv_obj_remove_event_cb_with_user_data(backend->region, deleted_region, backend);
        lv_obj_remove_event_cb_with_user_data(backend->region, video_not_opaque, backend);
        (void)xiaotai_ui_host_painter(host, backend->region, NULL, NULL);
        lv_obj_invalidate(backend->region);
    }
    backend->region = widget;
    backend->polled = backend->placed = false;
    if (widget) lv_obj_invalidate(widget);
    return XIAOTAI_OK;
}

static void mark_stale(ui_display_t *display)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    if (display->video_stale != UINT32_MAX) ++display->video_stale;
    xiaotai_mutex_unlock(&g_ui.lock);
}

static void source_state(ui_display_t *display, bool active)
{
    ui_lvgl_t *backend = display->lvgl;
    backend->polled = backend->placed = false;
    display->scene.video = active;
    if (backend->region) lv_obj_invalidate(backend->region);
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    display->power.redraw = true;
    xiaotai_mutex_unlock(&g_ui.lock);
}

static bool active_lvgl(ui_display_t *display)
{
    (void)xiaotai_mutex_lock(&g_ui.lock, XIAOTAI_WAIT_FOREVER);
    const bool ready = display->state == UI_DISPLAY_READY && !display->driver.stop &&
                       display->theme && display->theme->backend == XIAOTAI_UI_BACKEND_LVGL;
    xiaotai_mutex_unlock(&g_ui.lock);
    return ready;
}

xiaotai_err_t ui_video_install(uint8_t index, const xiaotai_ui_video_source_t *source,
                              uint32_t generation, void (*release)(void))
{
    if (!source || !source->pending || !source->paint || !generation) return XIAOTAI_ERR_INVALID_ARG;
    ui_display_t *display = ui_display_at(index);
    if (!display) return XIAOTAI_ERR_NOT_FOUND;
    if (!active_lvgl(display)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (display->driver.driven && ui_driver_context(display)) return XIAOTAI_ERR_INVALID_STATE;

    enter();
    ui_lvgl_t *backend = display->lvgl;
    if (!backend) {
        leave();
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (backend->generation && (int32_t)(generation - backend->generation) < 0) {
        mark_stale(display);
        leave();
        return XIAOTAI_ERR_INVALID_STATE;
    }
    if (!backend->region) {
        leave();
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (generation != backend->generation ||
        source->pending != backend->source.pending || source->paint != backend->source.paint ||
        source->placement != backend->source.placement || source->ctx != backend->source.ctx ||
        release != backend->release) {
        if (backend->release) backend->release();
        backend->generation = generation;
        backend->source = *source;
        backend->release = release;
        source_state(display, true);
    }
    leave();
    (void)ui_driver_kick(display);
    return XIAOTAI_OK;
}

void xiaotai_ui_video_clear(uint8_t index, uint32_t generation)
{
    ui_display_t *display = ui_display_at(index);
    if (!display || !generation || !active_lvgl(display) ||
        (display->driver.driven && ui_driver_context(display))) return;
    enter();
    ui_lvgl_t *backend = display->lvgl;
    if (!backend) {
        leave();
        return;
    }
    if (backend->generation && generation != backend->generation) {
        mark_stale(display);
    } else if (backend->source.pending) {
        if (backend->release) backend->release();
        backend->source = (xiaotai_ui_video_source_t){0};
        backend->release = NULL;
        source_state(display, false);
    }
    leave();
    (void)ui_driver_kick(display);
}

static void poll_video(ui_display_t *display, uint32_t now)
{
    ui_lvgl_t *backend = display->lvgl;
    xiaotai_ui_rect_t area, picture;
    if (!backend->source.pending || !video_scene(display) || !area_of(backend->region, &area)) return;
    if (backend->polled && (int32_t)(now - backend->due) < 0) return;
    backend->polled = true;
    backend->due = now + XIAOTAI_UI_VIDEO_POLL_MS;
    if (!backend->source.pending(backend->source.ctx)) return;

    if (backend->source.placement &&
        backend->source.placement(backend->source.ctx, area.x, area.y, area.width, area.height, &picture) &&
        picture.width && picture.height) {
        const lv_area_t placed = {.x1 = picture.x, .y1 = picture.y,
                                   .x2 = (int32_t)picture.x + picture.width - 1,
                                   .y2 = (int32_t)picture.y + picture.height - 1};
        lv_area_t dirty = placed;
        if (backend->placed) {
            if (backend->picture.x1 < dirty.x1) dirty.x1 = backend->picture.x1;
            if (backend->picture.y1 < dirty.y1) dirty.y1 = backend->picture.y1;
            if (backend->picture.x2 > dirty.x2) dirty.x2 = backend->picture.x2;
            if (backend->picture.y2 > dirty.y2) dirty.y2 = backend->picture.y2;
        }
        lv_obj_invalidate_area(backend->region, &dirty);
        backend->picture = placed;
        backend->placed = true;
    } else {
        lv_obj_invalidate(backend->region);
        backend->placed = false;
    }
}

void ui_lvgl_step(ui_display_t *display, uint32_t now)
{
    ui_lvgl_t *backend = display->lvgl;
    if (!backend) return;
    if (backend->pointer) lv_indev_read(backend->pointer);
    if (!display->power.awake) return;
    poll_video(display, now);
    if (ui_lvgl_port_ready(backend->root)) lv_refr_now(backend->root);
}

void ui_lvgl_invalidate_screen(const ui_display_t *display)
{
    const ui_lvgl_t *backend = display->lvgl;
    if (backend) lv_obj_invalidate(lv_display_get_screen_active(backend->root));
}

bool ui_lvgl_capture_flush(ui_display_t *display, const lv_area_t *area,
                          const uint8_t *pixels, size_t stride)
{
#if XIAOTAI_UI_CAPTURE
    ui_lvgl_t *backend = display->lvgl;
    xiaotai_ui_capture_t *capture = backend ? backend->capture : NULL;
    if (capture) {
        if (pixels == capture->pixels) return true; /* rendered straight to the screenshot */
        if (area->x1 >= 0 && area->y1 >= 0 && area->x2 >= area->x1 && area->y2 >= area->y1 &&
            area->x2 < capture->width && area->y2 < capture->height) {
            const size_t row = (size_t)lv_area_get_width(area) * 2U;
            for (int32_t y = area->y1; y <= area->y2; ++y) {
                const size_t dst = ((size_t)y * capture->width + (size_t)area->x1) * 2U;
                memcpy(capture->pixels + dst, pixels + (size_t)(y - area->y1) * stride, row);
            }
        }
        return true;
    }
#else
    (void)display;
    (void)area;
    (void)pixels;
    (void)stride;
#endif
    return false;
}

#if XIAOTAI_UI_CAPTURE
xiaotai_err_t ui_lvgl_capture(ui_display_t *display, xiaotai_ui_capture_t *out)
{
    ui_lvgl_t *backend = display->lvgl;
    if (!backend || display->part->desc.pixel != XIAOTAI_PIXEL_RGB565) return XIAOTAI_ERR_NOT_SUPPORTED;
    const uint16_t width = display->part->desc.width, height = display->part->desc.height;
    if (!width || !height || (uint64_t)width * height * 2U > UINT32_MAX ||
        height > SIZE_MAX / 2U / width) return XIAOTAI_ERR_INVALID_SIZE;
    const size_t bytes = (size_t)width * height * 2U;
    const size_t alignment = LV_DRAW_BUF_ALIGN > sizeof(void *) ? LV_DRAW_BUF_ALIGN : sizeof(void *);
    uint8_t *pixels = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, alignment, bytes);
    if (!pixels) return XIAOTAI_ERR_NO_MEM;
    lv_draw_buf_t frame;
    if (lv_draw_buf_init(&frame, width, height, LV_COLOR_FORMAT_RGB565, (uint32_t)width * 2U,
                         pixels, (uint32_t)bytes) != LV_RESULT_OK) {
        xiaotai_mem_free(pixels);
        return XIAOTAI_ERR_INVALID_SIZE;
    }
    memset(pixels, 0, bytes);
    *out = (xiaotai_ui_capture_t){.width = width, .height = height, .pixel = XIAOTAI_PIXEL_RGB565,
                                  .pixels = pixels, .size = bytes};
    lv_display_t *root = backend->root;
    lv_draw_buf_t *const first = root->buf_1, *const second = root->buf_2, *const active = root->buf_act;
    lv_draw_buf_t *const layer_buffer = root->layer_head->draw_buf;
    const lv_display_render_mode_t mode = root->render_mode;
    const bool auto_stride = root->stride_is_auto;
    const uint32_t invalidated = root->inv_p;
    lv_area_t areas[LV_INV_BUF_SIZE];
    uint8_t joined[LV_INV_BUF_SIZE];
    memcpy(areas, root->inv_areas, invalidated * sizeof(*areas));
    memcpy(joined, root->inv_area_joined, invalidated * sizeof(*joined));
    /* Do not draw into, modify or reorder the live borrowed scanout buffers. */
    lv_display_set_draw_buffers(root, &frame, NULL);
    lv_display_set_render_mode(root, LV_DISPLAY_RENDER_MODE_FULL);
    backend->capture = out;
    ui_lvgl_draw_capture(display, true);
    lv_obj_invalidate(lv_display_get_screen_active(root));
    lv_refr_now(root);
    ui_lvgl_draw_capture(display, false);
    backend->capture = NULL;
    lv_display_set_draw_buffers(root, first, second);
    root->buf_act = active;
    root->layer_head->draw_buf = layer_buffer;
    root->stride_is_auto = auto_stride;
    lv_display_set_render_mode(root, mode);
    /* Pending live updates still draw normally. A snapshot alone must not
     * invalidate the panel or leave it holding the temporary buffer. */
    root->inv_p = invalidated;
    memcpy(root->inv_areas, areas, invalidated * sizeof(*areas));
    memcpy(root->inv_area_joined, joined, invalidated * sizeof(*joined));
    return XIAOTAI_OK;
}
#endif
