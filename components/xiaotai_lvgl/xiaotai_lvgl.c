#include "xiaotai_lvgl.h"
#include "src/draw/sw/lv_draw_sw.h"

#define XIAOTAI_DRAW_UNIT 251
static xiaotai_image_draw_t s_image;
static lv_draw_unit_t *s_unit;

static int32_t evaluate(lv_draw_unit_t *unit, lv_draw_task_t *task)
{
    (void)unit;
    if (task->type != LV_DRAW_TASK_TYPE_IMAGE || task->preference_score < 80) return 0;
    task->preference_score = 80;
    task->preferred_draw_unit_id = XIAOTAI_DRAW_UNIT;
    return 1;
}

static int32_t dispatch(lv_draw_unit_t *unit, lv_layer_t *layer)
{
    lv_draw_task_t *task = lv_draw_get_next_available_task(layer, NULL, XIAOTAI_DRAW_UNIT);
    if (!task || task->preferred_draw_unit_id != XIAOTAI_DRAW_UNIT) return LV_DRAW_UNIT_IDLE;
    if (!lv_draw_layer_alloc_buf(layer)) return -1;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    task->draw_unit = unit;
    const lv_draw_image_dsc_t *image = task->draw_dsc;
    if (!s_image(task, image, &task->area)) lv_draw_sw_image(task, image, &task->area);
    task->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

esp_err_t xiaotai_lvgl_image_hook(xiaotai_image_draw_t draw)
{
    if (!draw) return ESP_ERR_INVALID_ARG;
    if (s_unit) return s_image == draw ? ESP_OK : ESP_ERR_INVALID_STATE;
    s_unit = lv_draw_create_unit(sizeof(*s_unit));
    if (!s_unit) return ESP_ERR_NO_MEM;
    s_image = draw;
    s_unit->name = "xiaotai-image";
    s_unit->evaluate_cb = evaluate;
    s_unit->dispatch_cb = dispatch;
    return ESP_OK;
}

bool xiaotai_lvgl_rgb565_surface(lv_draw_task_t *task, xiaotai_rgb565_surface_t *surface)
{
    if (!task || !surface || task->target_layer->color_format != LV_COLOR_FORMAT_RGB565) return false;
    lv_draw_buf_t *buffer = task->target_layer->draw_buf;
    if (!buffer || !buffer->data || (buffer->header.stride & 1U)) return false;
    *surface = (xiaotai_rgb565_surface_t){
        .buf = (uint16_t *)buffer->data, .buf_area = &task->target_layer->buf_area,
        .clip_area = &task->clip_area, .stride = buffer->header.stride / sizeof(uint16_t),
    };
    return true;
}
