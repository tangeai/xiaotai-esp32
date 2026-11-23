// SPDX-License-Identifier: MIT
#include "ui_lvgl_draw.h"
#include "xiaotai/port/mem.h"

#include "lvgl.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "src/misc/lv_area_private.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5 || LVGL_VERSION_PATCH != 0
#error The private LVGL draw task ABI is pinned to 9.5.0
#endif

#define UI_DRAW_UNIT 251U

typedef struct {
    lv_obj_t *widget;
    xiaotai_ui_paint_fn_t paint;
    xiaotai_ui_paint_prepare_fn_t prepare_fallback;
    void *ctx;
} painter_t;

typedef struct {
    painter_t entries[XIAOTAI_UI_PAINTERS_MAX];
    bool capture;
} painters_t;

static lv_draw_unit_t *unit;

static painter_t *find(ui_display_t *d, const lv_obj_t *widget)
{
    painters_t *list = d->drawing;
    if (!list) return NULL;
    for (size_t i = 0; i < XIAOTAI_UI_PAINTERS_MAX; ++i)
        if (list->entries[i].widget == widget) return &list->entries[i];
    return NULL;
}

static ui_display_t *for_widget(const lv_obj_t *widget)
{
    const lv_display_t *screen = lv_obj_get_display(widget);
    for (size_t i = 0; i < g_ui.display_count; ++i) {
        ui_display_t *d = ui_display_at(i);
        if (d && d->drawing && d->host.root == screen) return d;
    }
    return NULL;
}

static const painter_t *for_image(const lv_draw_image_dsc_t *image, ui_display_t **display)
{
    if (!image || !image->base.obj || image->base.part != LV_PART_MAIN) return NULL;
    lv_obj_t *widget = image->base.obj;
    ui_display_t *d = for_widget(widget);
    const painter_t *entry = d ? find(d, widget) : NULL;
    if (!entry || image->src != lv_image_get_src(widget)) return NULL;
    *display = d;
    return entry;
}

static void removed(lv_event_t *event)
{
    ui_display_t *d = lv_event_get_user_data(event);
    painter_t *entry = find(d, lv_event_get_current_target(event));
    if (entry) *entry = (painter_t){0};
}

static bool plain(const lv_draw_task_t *task, const lv_draw_image_dsc_t *image)
{
    if (!image || !image->base.obj || !task->target_layer || task->target_layer->color_format != LV_COLOR_FORMAT_RGB565)
        return false;
#if LV_DRAW_TRANSFORM_USE_MATRIX
    if (!lv_matrix_is_identity(&task->matrix)) return false;
#endif
    return task->opa == LV_OPA_COVER && !image->rotation &&
           image->scale_x == LV_SCALE_NONE && image->scale_y == LV_SCALE_NONE &&
           !image->skew_x && !image->skew_y && !image->tile && !image->clip_radius && !image->colorkey &&
           !image->bitmap_mask_src && image->opa == LV_OPA_COVER && image->recolor_opa == LV_OPA_TRANSP &&
           image->blend_mode == LV_BLEND_MODE_NORMAL;
}

static bool direct(const lv_draw_task_t *task, const lv_draw_image_dsc_t *image)
{
    if (!plain(task, image)) return false;
    /* The widget and its styled background can use the same source and part.
     * Keep both with LVGL when their descriptors cannot identify the owner. */
    return lv_obj_get_style_bg_image_src(image->base.obj, LV_PART_MAIN) != image->src ||
           lv_obj_get_style_bg_image_opa(image->base.obj, LV_PART_MAIN) <= LV_OPA_MIN;
}

static int32_t evaluate(lv_draw_unit_t *self, lv_draw_task_t *task)
{
    (void)self;
    if (task->type != LV_DRAW_TASK_TYPE_IMAGE) return 0;
    const lv_draw_image_dsc_t *image = task->draw_dsc;
    ui_display_t *d;
    const painter_t *entry = for_image(image, &d);
    if (!entry) return 0;
    painters_t *list = d->drawing;
    if (task->preference_score < 80 || !direct(task, image) || (list->capture && entry->prepare_fallback)) {
        if (entry->prepare_fallback) entry->prepare_fallback(entry->ctx);
        return 0;
    }
    task->preference_score = 80;
    task->preferred_draw_unit_id = UI_DRAW_UNIT;
    return 1;
}

static bool paint(lv_draw_task_t *task, const lv_draw_image_dsc_t *image, const ui_display_t *d, const painter_t *entry)
{
    if (!entry || !direct(task, image)) return false;
    const lv_layer_t *layer = task->target_layer;
    const lv_draw_buf_t *buf = layer->draw_buf;
    if (!buf || !buf->data || (buf->header.stride & 1U)) return false;
    const lv_area_t logical = {0, 0, d->part->desc.width - 1, d->part->desc.height - 1};
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &task->clip_area, &task->area) ||
        !lv_area_intersect(&clip, &clip, &layer->buf_area) || !lv_area_intersect(&clip, &clip, &logical)) return false;
    const size_t stride = buf->header.stride;
    const size_t x = (size_t)(clip.x1 - layer->buf_area.x1) * 2U;
    const size_t y = (size_t)(clip.y1 - layer->buf_area.y1);
    const size_t width = (size_t)lv_area_get_width(&clip), height = (size_t)lv_area_get_height(&clip);
    if (!stride || x + 2U * width > stride) return false;
    const uint64_t end = (uint64_t)(y + height - 1U) * stride + x + width * 2U;
    if (end > buf->data_size || task->area.x1 < INT16_MIN || task->area.x1 > INT16_MAX ||
        task->area.y1 < INT16_MIN || task->area.y1 > INT16_MAX) return false;

    const xiaotai_ui_surface_t target = {.pixels = buf->data + y * stride + x,
        .x = (uint16_t)clip.x1, .y = (uint16_t)clip.y1, .width = (uint16_t)width, .height = (uint16_t)height,
        .stride = (uint32_t)stride, .pixel = XIAOTAI_PIXEL_RGB565};
    return entry->paint(entry->ctx, &target, (int16_t)task->area.x1, (int16_t)task->area.y1);
}

static int32_t dispatch(lv_draw_unit_t *self, lv_layer_t *layer)
{
    lv_draw_task_t *task = lv_draw_get_next_available_task(layer, NULL, UI_DRAW_UNIT);
    if (!task || task->preferred_draw_unit_id != UI_DRAW_UNIT) return LV_DRAW_UNIT_IDLE;
    if (!lv_draw_layer_alloc_buf(layer)) return -1;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    task->draw_unit = self;
    const lv_draw_image_dsc_t *image = task->draw_dsc;
    ui_display_t *d = NULL;
    const painter_t *entry = for_image(image, &d);
    if (!paint(task, image, d, entry)) {
        if (entry && entry->prepare_fallback) entry->prepare_fallback(entry->ctx);
        lv_draw_sw_image(task, image, &task->area);
    }
    task->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

xiaotai_err_t ui_lvgl_draw_start(void)
{
    if (unit) return XIAOTAI_OK;
    unit = lv_draw_create_unit(sizeof(*unit));
    if (!unit) return XIAOTAI_ERR_NO_MEM;
    unit->name = "xt_ui_rgb565";
    unit->evaluate_cb = evaluate;
    unit->dispatch_cb = dispatch;
    return XIAOTAI_OK;
}

void ui_lvgl_draw_reset(void) { unit = NULL; }

void ui_lvgl_draw_capture(ui_display_t *d, bool active)
{
    painters_t *list = d->drawing;
    if (list) list->capture = active;
}

xiaotai_err_t ui_lvgl_draw_init(ui_display_t *d)
{
    d->drawing = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(painters_t));
    return d->drawing ? XIAOTAI_OK : XIAOTAI_ERR_NO_MEM;
}

void ui_lvgl_draw_drop(ui_display_t *d)
{
    painters_t *list = d->drawing;
    if (!list) return;
    for (size_t i = 0; i < XIAOTAI_UI_PAINTERS_MAX; ++i) {
        lv_obj_t *widget = list->entries[i].widget;
        if (widget) (void)lv_obj_remove_event_cb_with_user_data(widget, removed, d);
    }
    d->drawing = NULL;
    xiaotai_mem_free(list);
}

xiaotai_err_t xiaotai_ui_host_painter_ex(const xiaotai_ui_host_t *host, void *widget,
                                       xiaotai_ui_paint_fn_t fn,
                                       xiaotai_ui_paint_prepare_fn_t prepare_fallback, void *ctx)
{
    ui_display_t *d = host ? ui_display_at(host->index) : NULL;
    if (!d || host != &d->host || !widget) return XIAOTAI_ERR_INVALID_ARG;
    if (!d->drawing) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!ui_driver_context(d) || !lv_obj_is_valid(widget) || lv_obj_get_display(widget) != host->root ||
        !lv_obj_has_class(widget, &lv_image_class)) return XIAOTAI_ERR_INVALID_STATE;
    painter_t *entry = find(d, widget);
    if (!fn) {
        if (entry) {
            (void)lv_obj_remove_event_cb_with_user_data(widget, removed, d);
            *entry = (painter_t){0};
        }
    } else {
        if (!entry) {
            entry = find(d, NULL);
            if (!entry || !lv_obj_add_event_cb(widget, removed, LV_EVENT_DELETE, d)) return XIAOTAI_ERR_NO_MEM;
        }
        *entry = (painter_t){.widget = widget, .paint = fn, .prepare_fallback = prepare_fallback, .ctx = ctx};
    }
    lv_obj_invalidate(widget);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_ui_host_painter(const xiaotai_ui_host_t *host, void *widget,
                                      xiaotai_ui_paint_fn_t fn, void *ctx)
{
    return xiaotai_ui_host_painter_ex(host, widget, fn, NULL, ctx);
}
