// SPDX-License-Identifier: MIT
#include "esp_h264_dec_sw.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/video_backend.h"

#include <string.h>

/* The library allocates with the first picture, from the heap itself and
 * internal memory first: two tables of 216 bytes per macroblock, and small
 * ones. Internal memory is for the network and the audio: where the tables
 * would leave less than this, they are kept out of it. */
enum { INTERNAL_SPARE = 64 * 1024, MACROBLOCK_BYTES = 216, PLUGS = 3 };

typedef struct {
    esp_h264_dec_handle_t handle;
    esp_h264_dec_param_sw_handle_t parameters;
    void *plugs[PLUGS];     /* hold the internal blocks that would take a table, until the first picture */
    uint16_t width, height;
} h264_decoder_t;

static void unplug(h264_decoder_t *dec)
{
    for (size_t i = 0; i < PLUGS; ++i) {
        xiaotai_mem_free(dec->plugs[i]);
        dec->plugs[i] = NULL;
    }
}

/* Splits every internal block a table would fit in. */
static void plug(h264_decoder_t *dec)
{
    const size_t table = (size_t)((dec->width + 15U) / 16U) * ((dec->height + 15U) / 16U) * MACROBLOCK_BYTES;
    xiaotai_mem_stats_t memory;
    for (size_t i = 0; i < PLUGS; ++i) {
        xiaotai_mem_stats(XIAOTAI_MEM_INTERNAL, &memory);
        if (memory.largest_block < table || memory.free >= 2U * table + INTERNAL_SPARE) return;
        dec->plugs[i] = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, memory.largest_block - table + 1024U);
        if (!dec->plugs[i]) return;
    }
}

static xiaotai_err_t h264_error(esp_h264_err_t err)
{
    switch (err) {
    case ESP_H264_ERR_OK: return XIAOTAI_OK;
    case ESP_H264_ERR_MEM: return XIAOTAI_ERR_NO_MEM;
    case ESP_H264_ERR_ARG: return XIAOTAI_ERR_INVALID_ARG;
    case ESP_H264_ERR_OVERFLOW: return XIAOTAI_ERR_INVALID_SIZE;
    case ESP_H264_ERR_TIMEOUT: return XIAOTAI_ERR_TIMEOUT;
    case ESP_H264_ERR_UNSUPPORTED: return XIAOTAI_ERR_NOT_SUPPORTED;
    default: return XIAOTAI_FAIL;
    }
}

static bool crop_size(uint16_t width, uint16_t height)
{
    return width >= 16 && height >= 16 && !((width | height) & 1U) &&
           (size_t)width <= SIZE_MAX / height / 2U;
}

static int match(const xiaotai_video_decoder_config_t *c)
{
    return c && c->codec == XIAOTAI_VIDEO_CODEC_H264 &&
           ((!c->max_width && !c->max_height) || crop_size(c->max_width, c->max_height)) ? 100 : 0;
}

static xiaotai_err_t open_decoder(const xiaotai_video_decoder_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!c || c->codec != XIAOTAI_VIDEO_CODEC_H264) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!crop_size(c->max_width, c->max_height)) return XIAOTAI_ERR_INVALID_SIZE;
    h264_decoder_t *dec = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*dec));
    if (!dec) return XIAOTAI_ERR_NO_MEM;

    const esp_h264_dec_cfg_sw_t config = {.pic_type = ESP_H264_RAW_FMT_I420};
    xiaotai_err_t err = h264_error(esp_h264_dec_sw_new(&config, &dec->handle));
    /* The pinned library's allocation failure may return OK with no handle. */
    if (err == XIAOTAI_OK && !dec->handle) err = XIAOTAI_ERR_NO_MEM;
    if (err == XIAOTAI_OK) err = h264_error(esp_h264_dec_sw_get_param_hd(dec->handle, &dec->parameters));
    if (err == XIAOTAI_OK && !dec->parameters) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK) err = h264_error(esp_h264_dec_open(dec->handle));
    if (err != XIAOTAI_OK) {
        if (dec->handle) (void)esp_h264_dec_del(dec->handle);
        xiaotai_mem_free(dec);
        return err;
    }
    dec->width = c->max_width;
    dec->height = c->max_height;
    plug(dec);
    *state = dec;
    return XIAOTAI_OK;
}

static xiaotai_err_t copy_picture(const h264_decoder_t *dec, const esp_h264_dec_out_frame_t *pic, uint8_t *out)
{
    esp_h264_resolution_t res = {0};
    xiaotai_err_t err = h264_error(esp_h264_dec_get_resolution(dec->parameters, &res));
    if (err != XIAOTAI_OK) return err;
    const unsigned w = res.width, h = res.height;
    if (!pic->outbuf || !crop_size(res.width, res.height) || w < dec->width || h < dec->height)
        return XIAOTAI_ERR_INVALID_SIZE;
    const size_t pixels = (size_t)w * h;
    if (pixels + pixels / 2U > pic->out_size) return XIAOTAI_ERR_INVALID_SIZE;

    const unsigned x = ((w - dec->width) / 2U) & ~1U;
    const unsigned y = ((h - dec->height) / 2U) & ~1U;
    const uint8_t *plane = pic->outbuf;
    for (unsigned p = 0; p < 3; ++p) {
        const unsigned shift = p != 0;
        const size_t src_stride = w >> shift;
        const size_t dst_stride = dec->width >> shift;
        const unsigned rows = dec->height >> shift;
        const uint8_t *src = plane + (y >> shift) * src_stride + (x >> shift);
        if (src_stride == dst_stride) {
            memcpy(out, src, dst_stride * rows);
        } else {
            for (unsigned row = 0; row < rows; ++row)
                memcpy(out + row * dst_stride, src + row * src_stride, dst_stride);
        }
        plane += src_stride * (h >> shift);
        out += dst_stride * rows;
    }
    return XIAOTAI_OK;
}

static xiaotai_err_t decode(void *state, const xiaotai_video_packet_t *packet, uint8_t *out,
                           size_t capacity, xiaotai_video_image_t *image)
{
    h264_decoder_t *dec = state;
    if (!image) return XIAOTAI_ERR_INVALID_ARG;
    *image = (xiaotai_video_image_t){0};
    if (!dec || !packet || packet->codec != XIAOTAI_VIDEO_CODEC_H264 || !packet->data || !out)
        return XIAOTAI_ERR_INVALID_ARG;
    const size_t pixels = (size_t)dec->width * dec->height;
    const size_t output_bytes = pixels + pixels / 2U;
    if (!packet->size || packet->size > UINT32_MAX || capacity < output_bytes) return XIAOTAI_ERR_INVALID_SIZE;

    /* TinyH264 removes emulation-prevention bytes in the private input slot. */
    esp_h264_dec_in_frame_t input = {.raw_data = {.buffer = (uint8_t *)packet->data, .len = (uint32_t)packet->size},
        .pts = packet->timestamp_ms, .dts = packet->timestamp_ms};
    bool ready = false;
    while (input.raw_data.len) {
        esp_h264_dec_out_frame_t picture = {0};
        input.consume = 0;
        xiaotai_err_t err = h264_error(esp_h264_dec_process(dec->handle, &input, &picture));
        if (err != XIAOTAI_OK) return err;
        if (!input.consume || input.consume > input.raw_data.len) return XIAOTAI_FAIL;
        input.raw_data.buffer += input.consume;
        input.raw_data.len -= input.consume;
        if (picture.out_size) {
            unplug(dec);
            err = copy_picture(dec, &picture, out);
            if (err != XIAOTAI_OK) return err;
            ready = true;
        }
    }
    if (!ready) return XIAOTAI_ERR_NOT_FOUND;
    *image = (xiaotai_video_image_t){.pixel = XIAOTAI_VIDEO_PIXEL_I420, .data = out, .size = output_bytes,
        .width = dec->width, .height = dec->height, .rows = dec->height, .stride = dec->width};
    return XIAOTAI_OK;
}

static void close_decoder(void *state)
{
    h264_decoder_t *dec = state;
    if (!dec) return;
    unplug(dec);
    (void)esp_h264_dec_close(dec->handle);
    (void)esp_h264_dec_del(dec->handle);
    xiaotai_mem_free(dec);
}

const xiaotai_video_decoder_t video_esp_h264_decoder = {
    .name = "esp_h264_sw", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .flags = XIAOTAI_VIDEO_DECODER_BASELINE_ONLY | XIAOTAI_VIDEO_DECODER_MUTATES_INPUT,
    .output = XIAOTAI_VIDEO_PIXEL_I420, .output_alignment = 1,
    .stack_bytes = 16U * 1024U, .stack_internal = false,
    .match = match, .open = open_decoder, .decode = decode, .close = close_decoder,
};
