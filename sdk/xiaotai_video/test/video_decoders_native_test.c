// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "driver/jpeg_decode.h"
#include "esp_h264_dec_sw.h"
#include "sdkconfig.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/video_backend.h"

extern const xiaotai_video_decoder_t video_esp_h264_decoder;
extern const xiaotai_video_decoder_t video_esp_jpeg_decoder;
static const xiaotai_video_decoder_t *const h264 = &video_esp_h264_decoder;
static const xiaotai_video_decoder_t *const jpeg = &video_esp_jpeg_decoder;

static esp_h264_dec_t h264_handle;
static esp_h264_dec_param_t h264_params;
static esp_h264_resolution_t resolution = {.width = 22, .height = 26};
static unsigned opens, closes, deletes, allocations, consumed;
static bool alloc_failed, missing_handle, param_failed, open_failed, wrong_output_size;
static uint8_t i420[22U * 26U * 3U / 2U];

void *xiaotai_mem_calloc(xiaotai_mem_kind_t kind, size_t count, size_t size)
{
    assert(kind == XIAOTAI_MEM_INTERNAL);
    if (alloc_failed) return NULL;
    ++allocations;
    return calloc(count, size);
}

void xiaotai_mem_free(void *ptr) { assert(ptr && allocations); --allocations; free(ptr); }

esp_h264_err_t esp_h264_dec_sw_new(const esp_h264_dec_cfg_sw_t *config, esp_h264_dec_handle_t *handle)
{
    assert(config->pic_type == ESP_H264_RAW_FMT_I420);
    if (!missing_handle) *handle = &h264_handle;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_sw_get_param_hd(esp_h264_dec_handle_t handle, esp_h264_dec_param_sw_handle_t *params)
{
    assert(handle == &h264_handle);
    if (param_failed) return ESP_H264_ERR_FAIL;
    *params = &h264_params;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_open(esp_h264_dec_handle_t handle)
{
    assert(handle == &h264_handle);
    if (open_failed) return ESP_H264_ERR_TIMEOUT;
    ++opens;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_close(esp_h264_dec_handle_t handle)
{
    assert(handle == &h264_handle);
    ++closes;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_del(esp_h264_dec_handle_t handle)
{
    assert(handle == &h264_handle);
    ++deletes;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_get_resolution(esp_h264_dec_param_handle_t params, esp_h264_resolution_t *res)
{
    assert(params == &h264_params);
    *res = resolution;
    return ESP_H264_ERR_OK;
}

esp_h264_err_t esp_h264_dec_process(esp_h264_dec_handle_t handle, esp_h264_dec_in_frame_t *in,
                                   esp_h264_dec_out_frame_t *out)
{
    assert(handle == &h264_handle && in->raw_data.len && in->consume == 0);
    assert(in->pts == 17 && in->dts == 17);
    const uint8_t unit = *in->raw_data.buffer;
    *in->raw_data.buffer = 0xf0; /* Exercise the vendor's in-place byte editing. */
    ++consumed;
    in->consume = 1;
    switch (unit) {
    case 0: in->consume = 0; break;
    case 1: in->consume = in->raw_data.len + 1; break;
    case 2: return ESP_H264_ERR_MEM;
    case 3:
        out->outbuf = i420;
        out->out_size = wrong_output_size ? sizeof(i420) - 1U : sizeof(i420);
        break;
    default: break;
    }
    return ESP_H264_ERR_OK;
}

static void h264_test(void)
{
    xiaotai_video_decoder_config_t c = {.codec = XIAOTAI_VIDEO_CODEC_H264};
    assert(h264->match(&c) && !jpeg->match(&c));
    assert(h264->stack_bytes == 16384 && !h264->stack_internal && h264->load == XIAOTAI_VIDEO_LOAD_SOFTWARE);
    assert(h264->flags == (XIAOTAI_VIDEO_DECODER_BASELINE_ONLY | XIAOTAI_VIDEO_DECODER_MUTATES_INPUT));
    void *state = (void *)1;
    assert(h264->open(&c, &state) == XIAOTAI_ERR_INVALID_SIZE && !state);
    c.max_width = 17;
    c.max_height = 16;
    assert(!h264->match(&c));
    c.max_width = 16;
    alloc_failed = true;
    assert(h264->open(&c, &state) == XIAOTAI_ERR_NO_MEM && !state);
    alloc_failed = false;
    missing_handle = true;
    assert(h264->open(&c, &state) == XIAOTAI_ERR_NO_MEM && !state && !deletes && !allocations);
    missing_handle = false;
    param_failed = true;
    assert(h264->open(&c, &state) == XIAOTAI_FAIL && !state && deletes == 1 && !allocations);
    param_failed = false;
    open_failed = true;
    assert(h264->open(&c, &state) == XIAOTAI_ERR_TIMEOUT && !state && deletes == 2 && !allocations);
    open_failed = false;
    assert(h264->open(&c, &state) == XIAOTAI_OK && state && opens == 1);

    uint8_t au[] = {4, 5, 3}, actual[16U * 16U * 3U / 2U + 1U];
    for (size_t i = 0; i < sizeof(i420); ++i) i420[i] = (uint8_t)(i * 37U);
    const xiaotai_video_packet_t packet = {.codec = c.codec, .timestamp_ms = 17, .data = au, .size = sizeof(au)};
    xiaotai_video_image_t image;
    memset(actual, 0xff, sizeof(actual));
    assert(h264->decode(state, &packet, actual, sizeof(actual) - 2U, &image) == XIAOTAI_ERR_INVALID_SIZE && !consumed);
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_OK && consumed == sizeof(au));
    assert(au[0] == 0xf0 && au[1] == 0xf0 && au[2] == 0xf0 && actual[sizeof(actual) - 1] == 0xff);
    assert(image.pixel == XIAOTAI_VIDEO_PIXEL_I420 && !image.full_range && image.data == actual);
    assert(image.size == sizeof(actual) - 1U && image.width == 16 && image.height == 16 && image.stride == 16 && image.rows == 16);
    size_t s = 0, d = 0;
    for (unsigned p = 0; p < 3; ++p) {
        const unsigned shift = p != 0;
        for (unsigned y = 0; y < (16U >> shift); ++y) {
            for (unsigned x = 0; x < (16U >> shift); ++x)
                assert(actual[d++] == i420[s + (y + (4U >> shift)) * (22U >> shift) + x + (2U >> shift)]);
        }
        s += (22U >> shift) * (26U >> shift);
    }

    au[0] = 0;
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_FAIL && !image.data);
    au[0] = 1;
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_FAIL);
    au[0] = 2;
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_ERR_NO_MEM);
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_ERR_NOT_FOUND);
    au[0] = 3;
    resolution.width = 15;
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_ERR_INVALID_SIZE);
    au[0] = 3;
    resolution.width = 22;
    wrong_output_size = true;
    assert(h264->decode(state, &packet, actual, sizeof(actual), &image) == XIAOTAI_ERR_INVALID_SIZE);
    h264->close(state);
    assert(closes == 1 && deletes == 3 && allocations == 0);
}

static struct jpeg_decoder_t { int unused; } jpeg_handle;
static jpeg_decode_picture_info_t jpeg_picture;
static esp_err_t create_result, info_result, process_result;
static unsigned jpeg_opens, jpeg_closes, info_calls, process_calls, written_adjustment;
static const uint8_t compressed[] = {0xff, 0xd8, 0xff, 0xd9};
static _Alignas(128) uint8_t rgb[1024U * 608U * 2U];

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *config, jpeg_decoder_handle_t *state)
{
    assert(config->intr_priority == 0 && config->timeout_ms == 100);
    if (create_result != ESP_OK) return create_result;
    ++jpeg_opens;
    *state = &jpeg_handle;
    return ESP_OK;
}

esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t state)
{
    assert(state == &jpeg_handle);
    ++jpeg_closes;
    return ESP_OK;
}

esp_err_t jpeg_decoder_get_info(const uint8_t *data, uint32_t size, jpeg_decode_picture_info_t *picture)
{
    assert(data == compressed && size == sizeof(compressed));
    ++info_calls;
    *picture = jpeg_picture;
    return info_result;
}

esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t state, const jpeg_decode_cfg_t *config,
                               const uint8_t *data, uint32_t size, uint8_t *out, uint32_t out_size, uint32_t *written)
{
    assert(state == &jpeg_handle && config->output_format == JPEG_DECODE_OUT_FORMAT_RGB565);
    assert(config->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_RGB && config->conv_std == JPEG_YUV_RGB_CONV_STD_BT601);
    assert(data == compressed && size == sizeof(compressed) && out == rgb);
    assert(out_size == ((jpeg_picture.width + 15U) & ~15U) * ((jpeg_picture.height + 15U) & ~15U) * 2U);
    ++process_calls;
    out[0] = 0xf8;
    out[1] = 0;
    *written = out_size - written_adjustment;
    return process_result;
}

static void jpeg_test(void)
{
    static const xiaotai_video_decoder_config_t p4 = {XIAOTAI_VIDEO_CODEC_JPEG, 640, 480};
    static const xiaotai_video_decoder_config_t wt99 = {XIAOTAI_VIDEO_CODEC_JPEG, 1024, 608};
    const xiaotai_video_decoder_config_t c = {.codec = XIAOTAI_VIDEO_CODEC_JPEG};
    assert(jpeg->match(&c) && jpeg->match(&p4) && jpeg->match(&wt99) && !h264->match(&c));
    assert(jpeg->stack_internal && jpeg->stack_bytes == 8192 && jpeg->load == XIAOTAI_VIDEO_LOAD_HARDWARE);
    assert(jpeg->flags == XIAOTAI_VIDEO_DECODER_PERSISTENT && jpeg->output_alignment == CONFIG_CACHE_L2_CACHE_LINE_SIZE);
    assert(jpeg->output == XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED && !jpeg->decode);
    void *state;
    create_result = ESP_ERR_NO_MEM;
    assert(jpeg->open(&c, &state) == XIAOTAI_ERR_NO_MEM && !state);
    create_result = ESP_OK;
    assert(jpeg->open(&c, &state) == XIAOTAI_OK && state);
    const xiaotai_video_packet_t packet = {.codec = c.codec, .data = compressed, .size = sizeof(compressed)};
    xiaotai_video_image_t image;

    jpeg_picture.width = 800; jpeg_picture.height = 240;
    assert(jpeg->decode_configured(state, &p4, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_INVALID_SIZE && !process_calls);
    jpeg_picture.width = 639; jpeg_picture.height = 481;
    assert(jpeg->decode_configured(state, &p4, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_INVALID_SIZE && !process_calls);
    jpeg_picture.width = 480; jpeg_picture.height = 640;
    assert(jpeg->decode_configured(state, &p4, &packet, rgb + 1, sizeof(rgb) - 1U, &image) == XIAOTAI_ERR_INVALID_SIZE);
    assert(jpeg->decode_configured(state, &p4, &packet, rgb, 640U * 480U * 2U - 1U, &image) == XIAOTAI_ERR_INVALID_SIZE);
    assert(info_calls == 3 && !process_calls);
    assert(jpeg->decode_configured(state, &p4, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_OK);
    assert(image.width == 480 && image.height == 640 && image.rows == 640 && image.stride == 480);
    assert(image.size == 640U * 480U * 2U && image.data == rgb && image.pixel == jpeg->output);
    assert(image.data[0] == 0xf8 && image.data[1] == 0);
    jpeg_picture.width = 640; jpeg_picture.height = 480;
    assert(jpeg->decode_configured(state, &p4, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_OK);
    assert(image.rows == 480 && image.stride == 640);
    jpeg_picture.width = 1056; jpeg_picture.height = 240;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_INVALID_SIZE);
    assert(process_calls == 2);
    jpeg_picture.width = 1024; jpeg_picture.height = 600;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_OK);
    assert(image.rows == 608 && image.stride == 1024);
    jpeg_picture.width = 600; jpeg_picture.height = 1024;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_OK);
    assert(image.rows == 1024 && image.stride == 608);
    info_result = ESP_ERR_INVALID_ARG;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_INVALID_ARG);
    assert(!image.data && process_calls == 4);
    info_result = ESP_OK;
    process_result = ESP_ERR_TIMEOUT;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_TIMEOUT && !image.data);
    process_result = ESP_OK;
    written_adjustment = 2;
    assert(jpeg->decode_configured(state, &wt99, &packet, rgb, sizeof(rgb), &image) == XIAOTAI_ERR_INVALID_SIZE && !image.data);
    assert(jpeg_opens == 1 && !jpeg_closes);
    jpeg->close(state);
    assert(jpeg_closes == 1);
}

int main(void)
{
    h264_test();
    jpeg_test();
    return 0;
}
