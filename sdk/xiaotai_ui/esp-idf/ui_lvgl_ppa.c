// SPDX-License-Identifier: MIT
/* The pixel accelerator as an LVGL draw unit and buffer-copy handler.
 *
 * A display drawn in place keeps its two frame buffers in external memory.
 * Every pixel the software renderer moves there passes through the CPU's data
 * cache; the accelerator moves blocks between memories by DMA instead. Three
 * jobs are worth handing over, all of them whole rectangles of RGB565:
 *
 *   copy   LVGL brings the hidden frame buffer up to date with what the last
 *          frame changed (lv_draw_buf_copy)
 *   fill   an opaque rectangle of one colour (a page background)
 *   image  an opaque picture that is already RGB565 in memory
 *
 * Small rectangles stay with the software renderer: one accelerator job costs
 * a fixed setup and an interrupt, whatever its size. Anything the accelerator
 * refuses is drawn by the software renderer as well, so a refusal costs time,
 * never a picture.
 *
 * The driver invalidates the CPU cache over the rows a job writes, without
 * writing them back first (it does for a fill). Pixels the software renderer
 * left in the cache in those rows would be lost, so they are written back
 * here before every copy and image job. */
#include "ui_lvgl_ppa.h"

#if UI_LVGL_PPA

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "src/draw/lv_draw_buf_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/draw/sw/lv_draw_sw.h"
#include "src/misc/lv_area_private.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"

#if LVGL_VERSION_MAJOR != 9 || LVGL_VERSION_MINOR != 5 || LVGL_VERSION_PATCH != 0
#error The private LVGL draw task ABI is pinned to 9.5.0
#endif

#define UI_PPA_UNIT 252U
/* Better than the software renderer (100) and than the painter unit (80), which takes other tasks. */
#define UI_PPA_SCORE 70

enum {
    TARGETS = 4,
    /* Pixels below which a job is left to the software renderer (measured on the S31 bench, see
     * docs: the fixed cost of a job is about that of filling this many pixels in software). */
    FILL_MIN = 4096, COPY_MIN = 4096, IMAGE_MIN = 4096,
    CHECK_SIDE = 64, CHECK_BLOCK = 40, CHECK_AT = 12, CHECK_ALIGN = 128,
};

typedef enum { JOB_COPY = 0, JOB_FILL, JOB_IMAGE, JOB_KINDS } job_t;

static struct {
    ppa_client_handle_t srm, fill;
    lv_draw_unit_t *unit;
    lv_draw_buf_handlers_t handlers;         /* LVGL's own, with the copy replaced */
    lv_draw_buf_copy_cb_t software_copy;
    const lv_draw_buf_t *targets[TARGETS];
    bool clients, started, exact, off;       /* exact: a copy returns the very pixels it was given */
    bool checked;                            /* the check that sets `exact` has run */
    /* Counters: one writer (the UI task), read by a diagnostic. */
    atomic_uint jobs[JOB_KINDS], kilopixels[JOB_KINDS], micros[JOB_KINDS], software, refused;
    uint32_t odd[JOB_KINDS];                 /* pixels not yet counted as a thousand */
} ppa;

static void count(job_t kind, uint32_t pixels, int64_t began)
{
    const uint32_t total = ppa.odd[kind] + pixels;
    ppa.odd[kind] = total % 1000U;
    atomic_fetch_add_explicit(&ppa.jobs[kind], 1U, memory_order_relaxed);
    atomic_fetch_add_explicit(&ppa.kilopixels[kind], total / 1000U, memory_order_relaxed);
    atomic_fetch_add_explicit(&ppa.micros[kind], (unsigned)(xiaotai_time_us() - began), memory_order_relaxed);
}

static bool in_ram(const void *address)
{
    return address && (esp_ptr_external_ram(address) || esp_ptr_internal(address));
}

static bool is_target(const lv_draw_buf_t *buffer)
{
    if (!buffer || !buffer->data || ppa.off) return false;
    for (size_t i = 0; i < TARGETS; ++i)
        if (ppa.targets[i] && ppa.targets[i]->data == buffer->data) return true;
    return false;
}

static ppa_out_pic_blk_config_t out_of(const lv_draw_buf_t *buffer, uint32_t x, uint32_t y)
{
    return (ppa_out_pic_blk_config_t){.buffer = buffer->data, .buffer_size = buffer->data_size,
        .pic_w = buffer->header.stride / 2U, .pic_h = buffer->header.h, .block_offset_x = x, .block_offset_y = y,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565};
}

/* Copies a block of RGB565 between two pictures in memory. `pixels`, `stride`, `rows` describe the
 * source picture. */
static bool move(const uint8_t *pixels, uint32_t stride, uint32_t rows, uint32_t from_x, uint32_t from_y,
                 const lv_draw_buf_t *to, uint32_t to_x, uint32_t to_y, uint32_t width, uint32_t height)
{
    /* What the software renderer still holds in the cache of the rows about to be written. */
    const uint32_t to_stride = to->header.stride;
    (void)esp_cache_msync(to->data + (size_t)to_y * to_stride, (size_t)height * to_stride,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    const ppa_srm_oper_config_t job = {
        .in = {.buffer = pixels, .pic_w = stride / 2U, .pic_h = rows, .block_w = width, .block_h = height,
               .block_offset_x = from_x, .block_offset_y = from_y, .srm_cm = PPA_SRM_COLOR_MODE_RGB565},
        .out = out_of(to, to_x, to_y),
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0, .scale_x = 1.0f, .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(ppa.srm, &job) == ESP_OK) return true;
    atomic_fetch_add_explicit(&ppa.refused, 1U, memory_order_relaxed);
    return false;
}

/* -------------------------------------------------------------------- copy */

static void copy(lv_draw_buf_t *dest, const lv_area_t *dest_area, const lv_draw_buf_t *src, const lv_area_t *src_area)
{
    lv_area_t to = {0, 0, (int32_t)dest->header.w - 1, (int32_t)dest->header.h - 1};
    lv_area_t from = {0, 0, (int32_t)src->header.w - 1, (int32_t)src->header.h - 1};
    if (dest_area) to = *dest_area;
    if (src_area) from = *src_area;
    const int32_t width = lv_area_get_width(&to), height = lv_area_get_height(&to);
    const int64_t began = xiaotai_time_us();
    if (ppa.exact && is_target(dest) && in_ram(src->data) && dest->data != src->data &&
        dest->header.cf == LV_COLOR_FORMAT_RGB565 && src->header.cf == LV_COLOR_FORMAT_RGB565 &&
        width == lv_area_get_width(&from) && height == lv_area_get_height(&from) && width > 0 && height > 0 &&
        (uint32_t)width * (uint32_t)height >= COPY_MIN &&
        to.x1 >= 0 && to.y1 >= 0 && to.x2 < (int32_t)dest->header.w && to.y2 < (int32_t)dest->header.h &&
        from.x1 >= 0 && from.y1 >= 0 && from.x2 < (int32_t)src->header.w && from.y2 < (int32_t)src->header.h &&
        move(src->data, src->header.stride, src->header.h, (uint32_t)from.x1, (uint32_t)from.y1, dest,
             (uint32_t)to.x1, (uint32_t)to.y1, (uint32_t)width, (uint32_t)height)) {
        count(JOB_COPY, (uint32_t)width * (uint32_t)height, began);
        return;
    }
    ppa.software_copy(dest, dest_area, src, src_area);
}

/* --------------------------------------------------------------- draw unit */

/* The part of the task's area that is drawn, in the layer's buffer; false when it is not worth a job. */
static bool block_of(const lv_draw_task_t *task, uint32_t least, lv_area_t *block)
{
    const lv_layer_t *layer = task->target_layer;
    return lv_area_intersect(block, &task->clip_area, &task->area) && lv_area_intersect(block, block, &layer->buf_area) &&
           (uint32_t)lv_area_get_width(block) * (uint32_t)lv_area_get_height(block) >= least;
}

static bool plain(const lv_draw_task_t *task)
{
    const lv_layer_t *layer = task->target_layer;
    if (!layer || layer->color_format != LV_COLOR_FORMAT_RGB565 || !is_target(layer->draw_buf)) return false;
#if LV_DRAW_TRANSFORM_USE_MATRIX
    if (!lv_matrix_is_identity(&task->matrix)) return false;
#endif
    return task->opa == LV_OPA_COVER;
}

static bool fill_suits(const lv_draw_task_t *task)
{
    const lv_draw_fill_dsc_t *fill = task->draw_dsc;
    lv_area_t block;
    return fill && fill->opa >= LV_OPA_MAX && !fill->radius && fill->grad.dir == LV_GRAD_DIR_NONE &&
           block_of(task, FILL_MIN, &block);
}

static const lv_image_dsc_t *picture_of(const lv_draw_image_dsc_t *image)
{
    if (!image || !image->src || lv_image_src_get_type(image->src) != LV_IMAGE_SRC_VARIABLE) return NULL;
    const lv_image_dsc_t *picture = image->src;
    if (picture->header.cf != LV_COLOR_FORMAT_RGB565 || (picture->header.flags & LV_IMAGE_FLAGS_COMPRESSED) ||
        !picture->header.w || !picture->header.h || picture->header.stride < 2U * picture->header.w ||
        (picture->header.stride & 1U) || !in_ram(picture->data) ||
        (uint64_t)picture->header.stride * picture->header.h > picture->data_size) return NULL;
    return picture;
}

static bool image_suits(const lv_draw_task_t *task)
{
    const lv_draw_image_dsc_t *image = task->draw_dsc;
    lv_area_t block;
    return ppa.exact && image && !image->rotation && image->scale_x == LV_SCALE_NONE && image->scale_y == LV_SCALE_NONE &&
           !image->skew_x && !image->skew_y && !image->tile && !image->clip_radius && !image->colorkey &&
           !image->bitmap_mask_src && image->opa == LV_OPA_COVER && image->recolor_opa == LV_OPA_TRANSP &&
           image->blend_mode == LV_BLEND_MODE_NORMAL && picture_of(image) && block_of(task, IMAGE_MIN, &block);
}

static int32_t evaluate(lv_draw_unit_t *self, lv_draw_task_t *task)
{
    (void)self;
    if (task->preference_score <= UI_PPA_SCORE || !plain(task)) return 0;
    if (task->type == LV_DRAW_TASK_TYPE_FILL ? !fill_suits(task) :
        task->type == LV_DRAW_TASK_TYPE_IMAGE ? !image_suits(task) : true) return 0;
    task->preference_score = UI_PPA_SCORE;
    task->preferred_draw_unit_id = UI_PPA_UNIT;
    return 1;
}

static bool fill(const lv_draw_task_t *task)
{
    const lv_draw_fill_dsc_t *dsc = task->draw_dsc;
    const lv_layer_t *layer = task->target_layer;
    const lv_draw_buf_t *buffer = layer->draw_buf;
    lv_area_t block;
    if (!is_target(buffer) || !fill_suits(task) || !block_of(task, FILL_MIN, &block)) return false;
    const int64_t began = xiaotai_time_us();
    ppa_fill_oper_config_t job = {
        .out = out_of(buffer, (uint32_t)(block.x1 - layer->buf_area.x1), (uint32_t)(block.y1 - layer->buf_area.y1)),
        .fill_block_w = (uint32_t)lv_area_get_width(&block), .fill_block_h = (uint32_t)lv_area_get_height(&block),
        .fill_argb_color = {.a = 0xFF, .r = dsc->color.red, .g = dsc->color.green, .b = dsc->color.blue},
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    job.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
    if (ppa_do_fill(ppa.fill, &job) != ESP_OK) {
        atomic_fetch_add_explicit(&ppa.refused, 1U, memory_order_relaxed);
        return false;
    }
    count(JOB_FILL, job.fill_block_w * job.fill_block_h, began);
    return true;
}

static bool image(const lv_draw_task_t *task)
{
    const lv_layer_t *layer = task->target_layer;
    const lv_draw_buf_t *buffer = layer->draw_buf;
    lv_area_t block;
    if (!is_target(buffer) || !image_suits(task) || !block_of(task, IMAGE_MIN, &block)) return false;
    const lv_image_dsc_t *picture = picture_of(task->draw_dsc);
    /* The picture's first pixel lies on the task's top-left corner. */
    const int32_t x = block.x1 - task->area.x1, y = block.y1 - task->area.y1;
    const uint32_t width = (uint32_t)lv_area_get_width(&block), height = (uint32_t)lv_area_get_height(&block);
    if (x < 0 || y < 0 || (uint32_t)x + width > picture->header.w || (uint32_t)y + height > picture->header.h) return false;
    const int64_t began = xiaotai_time_us();
    if (!move(picture->data, picture->header.stride, picture->header.h, (uint32_t)x, (uint32_t)y, buffer,
              (uint32_t)(block.x1 - layer->buf_area.x1), (uint32_t)(block.y1 - layer->buf_area.y1), width, height))
        return false;
    count(JOB_IMAGE, width * height, began);
    return true;
}

static int32_t dispatch(lv_draw_unit_t *self, lv_layer_t *layer)
{
    lv_draw_task_t *task = lv_draw_get_next_available_task(layer, NULL, UI_PPA_UNIT);
    if (!task || task->preferred_draw_unit_id != UI_PPA_UNIT) return LV_DRAW_UNIT_IDLE;
    if (!lv_draw_layer_alloc_buf(layer)) return -1;
    task->state = LV_DRAW_TASK_STATE_IN_PROGRESS;
    task->draw_unit = self;
    if (task->type == LV_DRAW_TASK_TYPE_FILL) {
        if (!fill(task)) {
            atomic_fetch_add_explicit(&ppa.software, 1U, memory_order_relaxed);
            lv_draw_sw_fill(task, task->draw_dsc, &task->area);
        }
    } else if (!image(task)) {
        atomic_fetch_add_explicit(&ppa.software, 1U, memory_order_relaxed);
        lv_draw_sw_image(task, task->draw_dsc, &task->area);
    }
    task->state = LV_DRAW_TASK_STATE_FINISHED;
    lv_draw_dispatch_request();
    return 1;
}

/* --------------------------------------------------------------- lifecycle */

/* A copy and a fill between two small pictures of the kind the display uses: the accelerator is
 * trusted with pictures only when it returns exactly the pixels the software renderer would. */
static bool self_check(void)
{
    /* Whole cache lines of external memory, as the driver wants its output. */
    const size_t bytes = (size_t)CHECK_SIDE * CHECK_SIDE * 2U;
    uint16_t *from = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, CHECK_ALIGN, bytes);
    uint16_t *to = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, CHECK_ALIGN, bytes);
    bool same = from && to;
    if (same) {
        for (unsigned i = 0; i < CHECK_SIDE * CHECK_SIDE; ++i) {
            from[i] = (uint16_t)(i * 2654435761U >> 11);
            to[i] = 0x1234;
        }
        lv_draw_buf_t picture;
        same = lv_draw_buf_init(&picture, CHECK_SIDE, CHECK_SIDE, LV_COLOR_FORMAT_RGB565, CHECK_SIDE * 2U, to,
                                (uint32_t)bytes) == LV_RESULT_OK &&
               move((const uint8_t *)from, CHECK_SIDE * 2U, CHECK_SIDE, CHECK_AT + 3, CHECK_AT + 5, &picture, CHECK_AT,
                    CHECK_AT, CHECK_BLOCK, CHECK_BLOCK);
        for (unsigned y = 0; same && y < CHECK_SIDE; ++y)
            for (unsigned x = 0; x < CHECK_SIDE; ++x) {
                const bool inside = x >= CHECK_AT && x < CHECK_AT + CHECK_BLOCK && y >= CHECK_AT && y < CHECK_AT + CHECK_BLOCK;
                const uint16_t expected = inside ? from[(y + 5U) * CHECK_SIDE + x + 3U] : 0x1234;
                if (to[y * CHECK_SIDE + x] != expected) same = false;
            }
    }
    xiaotai_mem_free(from);
    xiaotai_mem_free(to);
    return same;
}

static xiaotai_err_t start(void)
{
    if (ppa.started) return XIAOTAI_OK;
    if (!ppa.clients) {
        /* Once per boot: the clients outlive LVGL. */
        const ppa_client_config_t srm = {.oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128};
        const ppa_client_config_t fill_client = {.oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1,
            .data_burst_length = PPA_DATA_BURST_LENGTH_128};
        if (ppa_register_client(&srm, &ppa.srm) != ESP_OK || !ppa.srm) return XIAOTAI_ERR_NOT_SUPPORTED;
        if (ppa_register_client(&fill_client, &ppa.fill) != ESP_OK || !ppa.fill) {
            (void)ppa_unregister_client(ppa.srm);
            ppa.srm = NULL;
            return XIAOTAI_ERR_NOT_SUPPORTED;
        }
        ppa.clients = true;
    }
    /* Idle until it is asked for: see ui_lvgl_ppa_enable. Not even its self-check runs before. */
    ppa.off = true;
    ppa.unit = lv_draw_create_unit(sizeof(*ppa.unit));
    if (!ppa.unit) return XIAOTAI_ERR_NO_MEM;
    ppa.unit->name = "xt_ui_ppa";
    ppa.unit->evaluate_cb = evaluate;
    ppa.unit->dispatch_cb = dispatch;
    ppa.handlers = *lv_draw_buf_get_handlers();
    ppa.software_copy = ppa.handlers.buf_copy_cb;
    ppa.handlers.buf_copy_cb = copy;
    ppa.started = true;
    return XIAOTAI_OK;
}

xiaotai_err_t ui_lvgl_ppa_attach(lv_draw_buf_t *buffers, size_t count)
{
    if (!buffers || !count) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_err_t err = start();
    if (err != XIAOTAI_OK) return err;
    size_t free_slots = 0;
    for (size_t i = 0; i < TARGETS; ++i) free_slots += !ppa.targets[i];
    if (free_slots < count) return XIAOTAI_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        /* The accelerator writes whole cache lines of memory it was told about, in external or internal RAM. */
        const lv_draw_buf_t *b = &buffers[i];
        const size_t line = in_ram(b->data) ? esp_cache_get_line_size_by_addr(b->data) : 0;
        if (!in_ram(b->data) || b->header.cf != LV_COLOR_FORMAT_RGB565 || (b->header.stride & 1U) ||
            (line && (((uintptr_t)b->data | b->data_size) & (line - 1U)))) return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    for (size_t i = 0; i < count; ++i) {
        for (size_t slot = 0; slot < TARGETS; ++slot)
            if (!ppa.targets[slot]) { ppa.targets[slot] = &buffers[i]; break; }
        buffers[i].handlers = &ppa.handlers;
    }
    return XIAOTAI_OK;
}

void ui_lvgl_ppa_detach(lv_draw_buf_t *buffers, size_t count)
{
    for (size_t i = 0; buffers && i < count; ++i)
        for (size_t slot = 0; slot < TARGETS; ++slot)
            if (ppa.targets[slot] == &buffers[i]) {
                ppa.targets[slot] = NULL;
                buffers[i].handlers = lv_draw_buf_get_handlers();
            }
}

void ui_lvgl_ppa_reset(void)
{
    ppa.unit = NULL;
    ppa.started = false;
    memset(ppa.targets, 0, sizeof(ppa.targets));
}

/* The accelerator starts idle and is turned on by the bench (AT+UI ACCEL 1), not by a product. On
 * the one board it has run on (ESP32-S31, an RGB panel fed from frame buffers in external RAM) its
 * jobs make the panel lose pixels: the stream the panel is fed slips against its sync, the picture
 * wraps round both ways and, with a job in every frame, rolls. The RGB driver has no way on that
 * chip to line the stream up again, so one slip stays until a restart. Until the cause is found it
 * is a thing to measure with, not to ship. */
xiaotai_err_t ui_lvgl_ppa_enable(bool on)
{
    if (!ppa.started) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (on && !ppa.checked) {
        ppa.exact = self_check();
        ppa.checked = true;
        if (!ppa.exact) XIAOTAI_LOGW("ui_ppa", "copies differ from the source: pictures stay with the software renderer");
    }
    ppa.off = !on;
    return XIAOTAI_OK;
}

void ui_lvgl_ppa_report(char *words, size_t capacity)
{
    if (!words || !capacity) return;
    words[0] = '\0';
    if (!ppa.started) return;
#define N(counter) ((unsigned)atomic_load_explicit(&(counter), memory_order_relaxed))
    (void)snprintf(words, capacity, "on=%u exact=%u copy=%u/%uk/%uus fill=%u/%uk/%uus image=%u/%uk/%uus software=%u refused=%u",
                   !ppa.off, ppa.exact, N(ppa.jobs[JOB_COPY]), N(ppa.kilopixels[JOB_COPY]), N(ppa.micros[JOB_COPY]),
                   N(ppa.jobs[JOB_FILL]), N(ppa.kilopixels[JOB_FILL]), N(ppa.micros[JOB_FILL]),
                   N(ppa.jobs[JOB_IMAGE]), N(ppa.kilopixels[JOB_IMAGE]), N(ppa.micros[JOB_IMAGE]),
                   N(ppa.software), N(ppa.refused));
#undef N
}

#endif
