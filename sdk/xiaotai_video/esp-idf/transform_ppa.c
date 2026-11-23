// SPDX-License-Identifier: MIT
#include "driver/ppa.h"
#include "esp_cache.h"
#include "geometry.h"
#include "sdkconfig.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"

#include <string.h>

enum {
    CACHE_ALIGNMENT = CONFIG_CACHE_L1_CACHE_LINE_SIZE > CONFIG_CACHE_L2_CACHE_LINE_SIZE
                      ? CONFIG_CACHE_L1_CACHE_LINE_SIZE : CONFIG_CACHE_L2_CACHE_LINE_SIZE,
    DIMENSION_MAX = 0x3fff, /* Width, height and offset fields in a DMA2D descriptor. */
    SCALE_DENOMINATOR = 16,
    SCALE_MAX = 255, /* Retain the display converter's range (in units of 1/16). */
};
_Static_assert(CACHE_ALIGNMENT > 0 && !(CACHE_ALIGNMENT & (CACHE_ALIGNMENT - 1)), "invalid PPA cache alignment");

typedef struct {
    xiaotai_video_transform_config_t layout;
    ppa_srm_oper_config_t operation;
    ppa_client_handle_t srm, fill;
    uint8_t *packed;
    size_t input_bytes, output_bytes, packed_capacity;
    bool clear;
} transform_t;

static bool limits(xiaotai_video_pixel_t in, xiaotai_video_pixel_t out, xiaotai_video_scale_limits_t *scale)
{
    const bool paired = in == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    const bool output_paired = out == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED;
    if (!scale || (!paired && in != XIAOTAI_VIDEO_PIXEL_I420 && !video_geometry_rgb_bytes(in)) ||
        ((!output_paired || !paired) && !video_geometry_rgb_bytes(out))) return false;
    /* The hardware discards the least significant scale bit for paired output. */
    *scale = (xiaotai_video_scale_limits_t){SCALE_DENOMINATOR, output_paired ? 2 : 1, SCALE_MAX};
    return true;
}

static bool fits(xiaotai_video_rect_t r, uint16_t width, uint16_t height)
{
    return r.width && r.height && !((r.x | r.y | r.width | r.height) & 1U) &&
           (uint32_t)r.x + r.width <= width && (uint32_t)r.y + r.height <= height;
}

/* The native block size truncates at each axis. Find one common discrete
 * step that produces precisely the engine's planned output on both axes. */
static unsigned scale_step(const xiaotai_video_transform_config_t *c, unsigned step)
{
    const bool turn = (c->rotation & 1U) != 0;
    const unsigned width = turn ? c->place.height : c->place.width;
    const unsigned height = turn ? c->place.width : c->place.height;
    const uint32_t x = ((uint32_t)width * SCALE_DENOMINATOR + c->crop.width - 1U) / c->crop.width;
    const uint32_t y = ((uint32_t)height * SCALE_DENOMINATOR + c->crop.height - 1U) / c->crop.height;
    const uint32_t n = ((x > y ? x : y) + step - 1U) / step * step;
    if (!n || n > SCALE_MAX || (uint32_t)c->crop.width * n / SCALE_DENOMINATOR != width ||
        (uint32_t)c->crop.height * n / SCALE_DENOMINATOR != height) return 0;
    return (unsigned)n;
}

static unsigned supported_step(const xiaotai_video_transform_config_t *c)
{
    xiaotai_video_scale_limits_t scale;
    if (!c || !limits(c->in_pixel, c->out_pixel, &scale) || (unsigned)c->rotation > XIAOTAI_VIDEO_ROTATE_270 ||
        c->in_stride < c->in_width || c->in_stride > DIMENSION_MAX || c->in_height > DIMENSION_MAX ||
        c->out_width > DIMENSION_MAX || c->out_height > DIMENSION_MAX ||
        !fits(c->crop, c->in_width, c->in_height) || !fits(c->place, c->out_width, c->out_height) ||
        !video_geometry_image_bytes(c->in_pixel, c->in_stride, c->in_height) ||
        !video_geometry_image_bytes(c->out_pixel, c->out_width, c->out_height)) return 0;
    if ((c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420 || c->in_pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED) &&
        c->in_stride != c->in_width) return 0;
    if (c->fill && c->out_pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED &&
        (c->place.width != c->out_width || c->place.height != c->out_height)) return 0;
    return scale_step(c, scale.step);
}

static int match(const xiaotai_video_transform_config_t *c) { return supported_step(c) ? 10 : 0; }

static ppa_srm_color_mode_t native_pixel(xiaotai_video_pixel_t pixel)
{
    switch (pixel) {
    case XIAOTAI_VIDEO_PIXEL_RGB565:
    case XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED: return PPA_SRM_COLOR_MODE_RGB565;
    case XIAOTAI_VIDEO_PIXEL_RGB888: return PPA_SRM_COLOR_MODE_RGB888;
    default: return PPA_SRM_COLOR_MODE_YUV420;
    }
}

static size_t aligned_bytes(size_t bytes)
{
    return (bytes + CACHE_ALIGNMENT - 1U) & ~(size_t)(CACHE_ALIGNMENT - 1U);
}

static xiaotai_err_t register_client(ppa_operation_t kind, ppa_client_handle_t *out)
{
    const ppa_client_config_t client = {
        .oper_type = kind, .max_pending_trans_num = 1, .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    const esp_err_t result = ppa_register_client(&client, out);
    return result == ESP_OK && !*out ? XIAOTAI_FAIL : result;
}

static void unregister_client(ppa_client_handle_t client)
{
    if (!client) return;
    const esp_err_t err = ppa_unregister_client(client);
    if (err != ESP_OK) XIAOTAI_LOGW("video_ppa", "client release failed: 0x%x", (unsigned)err);
}

static void close(void *state)
{
    transform_t *t = state;
    if (!t) return;
    unregister_client(t->fill);
    unregister_client(t->srm);
    xiaotai_mem_free(t->packed);
    xiaotai_mem_free(t);
}

static void configure_plan(transform_t *t, const xiaotai_video_transform_config_t *c, unsigned step)
{
    t->layout = *c;
    t->input_bytes = video_geometry_image_bytes(c->in_pixel, c->in_stride, c->in_height);
    t->output_bytes = video_geometry_image_bytes(c->out_pixel, c->out_width, c->out_height);
    t->clear = c->fill && (c->place.width != c->out_width || c->place.height != c->out_height);
    const bool repack = c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420;
    static const ppa_srm_rotation_angle_t rotation[] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_270,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_90,
    };
    const ppa_color_range_t range = c->in_full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT;
    t->operation = (ppa_srm_oper_config_t){
        .in = {.pic_w = repack ? c->crop.width : c->in_stride, .pic_h = repack ? c->crop.height : c->in_height,
               .block_w = c->crop.width, .block_h = c->crop.height,
               .block_offset_x = repack ? 0 : c->crop.x, .block_offset_y = repack ? 0 : c->crop.y,
               .srm_cm = native_pixel(c->in_pixel), .yuv_range = range, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .out = {.pic_w = c->out_width, .pic_h = c->out_height,
                .block_offset_x = c->place.x, .block_offset_y = c->place.y,
                .srm_cm = native_pixel(c->out_pixel), .yuv_range = range, .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601},
        .rotation_angle = rotation[c->rotation], .scale_x = (float)step / SCALE_DENOMINATOR,
        .scale_y = (float)step / SCALE_DENOMINATOR, .byte_swap = c->in_pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
}

static xiaotai_err_t open(const xiaotai_video_transform_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    const unsigned step = supported_step(c);
    if (!step) return XIAOTAI_ERR_NOT_SUPPORTED;
    transform_t *t = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*t));
    if (!t) return XIAOTAI_ERR_NO_MEM;
    configure_plan(t, c, step);
    if (c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420) {
        /* Reserve for the whole declared visible picture, even when the
         * first plan uses only a crop. Later plans reuse the same storage. */
        t->packed_capacity = t->input_bytes;
        t->packed = xiaotai_mem_aligned(XIAOTAI_MEM_BULK, CACHE_ALIGNMENT, aligned_bytes(t->packed_capacity));
        if (!t->packed) { close(t); return XIAOTAI_ERR_NO_MEM; }
    }
    const xiaotai_err_t err = register_client(PPA_OPERATION_SRM, &t->srm);
    if (err != XIAOTAI_OK) { close(t); return err; }
    /* RGB-to-RGB clears through DMA when available. The YUV conversion path
     * retains its CPU clear and write-back before running SRM. */
    if (t->clear && video_geometry_rgb_bytes(c->in_pixel)) {
        const xiaotai_err_t result = register_client(PPA_OPERATION_FILL, &t->fill);
        if (result != XIAOTAI_OK) {
            unregister_client(t->fill);
            t->fill = NULL;
            XIAOTAI_LOGW("video_ppa", "fill unavailable (0x%x); using CPU clear", (unsigned)result);
        }
    }
    *state = t;
    return XIAOTAI_OK;
}

static xiaotai_err_t reconfigure(void *state, const xiaotai_video_transform_config_t *c)
{
    transform_t *t = state;
    if (!t || !c) return XIAOTAI_ERR_INVALID_ARG;
    const unsigned step = supported_step(c);
    if (!step) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (c->in_pixel == XIAOTAI_VIDEO_PIXEL_I420 &&
        video_geometry_image_bytes(c->in_pixel, c->crop.width, c->crop.height) > t->packed_capacity)
        return XIAOTAI_ERR_NOT_SUPPORTED;
    configure_plan(t, c, step);
    return XIAOTAI_OK;
}

static void pack_i420(const xiaotai_video_transform_config_t *c, const uint8_t *in, uint8_t *packed)
{
    const size_t pixels = (size_t)c->in_stride * c->in_height;
    const uint8_t *u = in + pixels, *v = u + pixels / 4U;
    for (size_t y = c->crop.y; y < (size_t)c->crop.y + c->crop.height; ++y) {
        const uint8_t *luma = in + y * c->in_stride + c->crop.x;
        const uint8_t *chroma = (y & 1U ? v : u) + y / 2U * (c->in_stride / 2U) + c->crop.x / 2U;
        for (size_t x = 0; x < c->crop.width; x += 2) {
            *packed++ = *chroma++;
            *packed++ = luma[x];
            *packed++ = luma[x + 1U];
        }
    }
}

static xiaotai_err_t clear_background(const transform_t *t, uint8_t *out, size_t dma_bytes)
{
    if (!t->fill || !video_geometry_rgb_bytes(t->layout.in_pixel)) {
        memset(out, 0, t->output_bytes);
        return esp_cache_msync(out, dma_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    const uint16_t width = t->layout.out_width, height = t->layout.out_height;
    const ppa_fill_oper_config_t fill = {
        .out = {.buffer = out, .buffer_size = dma_bytes, .pic_w = width, .pic_h = height,
                .fill_cm = t->layout.out_pixel == XIAOTAI_VIDEO_PIXEL_RGB888
                           ? PPA_FILL_COLOR_MODE_RGB888 : PPA_FILL_COLOR_MODE_RGB565},
        .fill_block_w = width, .fill_block_h = height, .fill_color_val = 0, .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_fill(t->fill, &fill);
}

static void swap_output_words(const xiaotai_video_transform_config_t *c, uint8_t *out)
{
    for (size_t y = c->place.y; y < (size_t)c->place.y + c->place.height; ++y) {
        uint8_t *row = out + (y * c->out_width + c->place.x) * 2U;
        for (size_t x = 0; x < (size_t)c->place.width * 2U; x += 2) {
            const uint8_t low = row[x];
            row[x] = row[x + 1U];
            row[x + 1U] = low;
        }
    }
}

static xiaotai_err_t run(void *state, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_capacity)
{
    const transform_t *t = state;
    if (!t || !in || !out || (uintptr_t)out % CACHE_ALIGNMENT) return XIAOTAI_ERR_INVALID_ARG;
    const size_t dma_bytes = aligned_bytes(t->output_bytes);
    if (in_size < t->input_bytes || out_capacity < dma_bytes || out_capacity % CACHE_ALIGNMENT)
        return XIAOTAI_ERR_INVALID_SIZE;
    const uintptr_t a = (uintptr_t)in, b = (uintptr_t)out;
    if ((b >= a && b - a < in_size) || (a >= b && a - b < dma_bytes)) return XIAOTAI_ERR_INVALID_ARG;
    if (t->clear) {
        const xiaotai_err_t err = clear_background(t, out, dma_bytes);
        if (err != XIAOTAI_OK) return err;
    }
    const bool repack = t->layout.in_pixel == XIAOTAI_VIDEO_PIXEL_I420;
    if (repack) pack_i420(&t->layout, in, t->packed);
    ppa_srm_oper_config_t operation = t->operation;
    operation.in.buffer = repack ? t->packed : in;
    operation.out.buffer = out;
    operation.out.buffer_size = dma_bytes;
    const esp_err_t err = ppa_do_scale_rotate_mirror(t->srm, &operation);
    if (err != ESP_OK) return err;
    if (t->layout.out_pixel == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED) swap_output_words(&t->layout, out);
    return XIAOTAI_OK;
}

const xiaotai_video_transform_t video_esp_ppa_transform = {
    .name = "ppa", .load = XIAOTAI_VIDEO_LOAD_HARDWARE, .output_alignment = CACHE_ALIGNMENT,
    .match = match, .limits = limits, .open = open, .run = run, .close = close,
    .reconfigure = reconfigure,
};
