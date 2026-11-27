// SPDX-License-Identifier: MIT
#include "ui_lvgl_draw.h"
#include "xiaotai/port/mem.h"

#include "lvgl.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/core/lv_obj_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "src/misc/lv_area_private.h"
#include "lvgl/ui_lvgl_private.h"

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
    uint8_t *strip;         /* xiaotai_ui_host_paint(): a few rows of an area */
} painters_t;

/* Some dozen rows of a small area; on a large display areas are larger. */
static size_t strip_bytes(const xiaotai_display_desc_t *desc)
{
    return (uint32_t)desc->width * desc->height > 320U * 240U ? 16384U : 4096U;
}

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
    xiaotai_mem_free(list->strip);
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

/* ------------------------------------------------------------- at once */

/* Whether the object, or one in it, puts a pixel into the area. Only what is
 * known to draw nothing is let through. */
static bool draws_over(const lv_obj_t *obj, const lv_area_t *area)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t own, shared;
    lv_obj_get_coords(obj, &own);
    const int32_t margin = lv_obj_get_ext_draw_size(obj);
    lv_area_increase(&own, margin, margin);
    const bool touches = lv_area_intersect(&shared, &own, area);
    if (!touches && !lv_obj_has_flag(obj, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) return false;
    if (touches) {
        const bool label = lv_obj_check_type(obj, &lv_label_class);
        if (!label && !lv_obj_check_type(obj, &lv_obj_class) && !lv_obj_check_type(obj, &lv_button_class)) return true;
        if (label && lv_label_get_text(obj)[0]) return true;
        if (lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) > LV_OPA_MIN || lv_obj_get_style_bg_image_src(obj, LV_PART_MAIN) ||
            (lv_obj_get_style_border_width(obj, LV_PART_MAIN) && lv_obj_get_style_border_opa(obj, LV_PART_MAIN) > LV_OPA_MIN) ||
            (lv_obj_get_style_outline_width(obj, LV_PART_MAIN) && lv_obj_get_style_outline_opa(obj, LV_PART_MAIN) > LV_OPA_MIN) ||
            (lv_obj_get_style_shadow_width(obj, LV_PART_MAIN) && lv_obj_get_style_shadow_opa(obj, LV_PART_MAIN) > LV_OPA_MIN)) return true;
    }
    const uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i)
        if (draws_over(lv_obj_get_child(obj, (int32_t)i), area)) return true;
    return false;
}

/* The widget's own pixels, and only they, are what the panel shows there. */
static bool shows_alone(const ui_display_t *d, lv_obj_t *widget, const lv_area_t *area)
{
    lv_area_t coords;
    if (lv_obj_get_style_opa_recursive(widget, LV_PART_MAIN) < LV_OPA_COVER || lv_image_get_rotation(widget) ||
        lv_image_get_scale_x(widget) != LV_SCALE_NONE || lv_image_get_scale_y(widget) != LV_SCALE_NONE ||
        lv_obj_get_style_image_opa(widget, LV_PART_MAIN) < LV_OPA_COVER || lv_obj_get_style_image_recolor_opa(widget, LV_PART_MAIN) > LV_OPA_MIN ||
        lv_obj_get_style_blend_mode(widget, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL || lv_obj_get_style_radius(widget, LV_PART_MAIN) ||
        lv_obj_get_style_bitmap_mask_src(widget, LV_PART_MAIN) || lv_obj_get_style_clip_corner(widget, LV_PART_MAIN)) return false;
    for (const lv_obj_t *obj = widget; obj; obj = lv_obj_get_parent(obj)) {
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_layer_type(obj) != LV_LAYER_TYPE_NONE) return false;
        lv_obj_get_coords(obj, &coords);
        if (!lv_area_is_in(area, &coords, 0)) return false;
        const lv_obj_t *parent = lv_obj_get_parent(obj);
        if (!parent) {
            if (obj != lv_display_get_screen_active(d->host.root) || lv_display_get_screen_prev(d->host.root)) return false;
            break;
        }
        const uint32_t count = lv_obj_get_child_count(parent);
        for (uint32_t i = lv_obj_get_index(obj) + 1; i < count; ++i)
            if (draws_over(lv_obj_get_child(parent, (int32_t)i), area)) return false;
    }
    lv_obj_t *const above[] = {lv_display_get_layer_top(d->host.root), lv_display_get_layer_sys(d->host.root)};
    for (size_t k = 0; k < sizeof(above) / sizeof(above[0]); ++k) {
        const uint32_t count = above[k] ? lv_obj_get_child_count(above[k]) : 0;
        for (uint32_t i = 0; i < count; ++i)
            if (draws_over(lv_obj_get_child(above[k], (int32_t)i), area)) return false;
    }
    return true;
}

/* One area, in as many pieces as the strip needs. */
static bool paint_now(ui_display_t *d, const painter_t *entry, uint8_t *strip, const lv_area_t *coords, const lv_area_t *area)
{
    const xiaotai_display_desc_t *desc = &d->part->desc;
    const size_t row = (size_t)lv_area_get_width(area) * 2U;
    const int32_t rows = (int32_t)(strip_bytes(desc) / row);
    for (int32_t y = area->y1; y <= area->y2; y += rows) {
        lv_area_t piece = {area->x1, y, area->x2, y + rows - 1};
        if (piece.y2 > area->y2) piece.y2 = area->y2;
        const xiaotai_ui_surface_t target = {.pixels = strip, .x = (uint16_t)piece.x1, .y = (uint16_t)piece.y1,
            .width = (uint16_t)lv_area_get_width(&piece), .height = (uint16_t)lv_area_get_height(&piece), .stride = (uint32_t)row,
            .pixel = XIAOTAI_PIXEL_RGB565, .swap_bytes = (desc->flags & XIAOTAI_DISPLAY_SWAP_BYTES) != 0};
        if (!entry->paint(entry->ctx, &target, (int16_t)coords->x1, (int16_t)coords->y1) || !ui_lvgl_port_push(d, &piece, strip)) return false;
    }
    return true;
}

uint32_t xiaotai_ui_host_paint(const xiaotai_ui_host_t *host, void *widget, const xiaotai_ui_rect_t *areas, size_t count)
{
    ui_display_t *d = host ? ui_display_at(host->index) : NULL;
    if (!d || host != &d->host || !widget || !d->drawing || !ui_driver_context(d)) return 0;
    const xiaotai_display_desc_t *desc = &d->part->desc;
    painters_t *list = d->drawing;
    const painter_t *entry = find(d, widget);
    if (!entry || list->capture || desc->render != XIAOTAI_RENDER_PARTIAL || desc->pixel != XIAOTAI_PIXEL_RGB565 || desc->rotation) return 0;
    if (!list->strip) {
        const size_t align = desc->buffer_align > sizeof(void *) ? desc->buffer_align : sizeof(void *);
        list->strip = xiaotai_mem_aligned(XIAOTAI_MEM_DMA, align, strip_bytes(desc));
        if (!list->strip) return 0;
    }
    lv_area_t coords;
    const lv_area_t screen = {0, 0, desc->width - 1, desc->height - 1};
    lv_obj_get_coords(widget, &coords);
    uint32_t shown = 0;
    for (size_t i = 0; i < count && i < 32U; ++i) {
        lv_area_t area = {areas[i].x, areas[i].y, (int32_t)areas[i].x + areas[i].width - 1, (int32_t)areas[i].y + areas[i].height - 1};
        /* What the panel asks of an area, as for a refresh. */
        (void)lv_display_send_event(d->host.root, LV_EVENT_INVALIDATE_AREA, &area);
        if (!lv_area_is_in(&area, &coords, 0) || !lv_area_is_in(&area, &screen, 0) ||
            (size_t)lv_area_get_width(&area) * 2U > strip_bytes(desc) || !shows_alone(d, widget, &area)) continue;
        if (paint_now(d, entry, list->strip, &coords, &area)) shown |= UINT32_C(1) << i;
    }
    return shown;
}
