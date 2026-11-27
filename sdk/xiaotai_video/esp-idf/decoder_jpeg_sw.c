// SPDX-License-Identifier: MIT
#include <limits.h>
#include "esp_jpeg_dec.h"
#include "xiaotai/video_backend.h"

enum {
    JPEG_ALIGNMENT = 16,
    JPEG_EDGE_ALIGNMENT = 16,   /* the budget of a picture counts both sides rounded up to it */
};

static xiaotai_err_t jpeg_result(jpeg_error_t result)
{
    switch (result) {
    case JPEG_ERR_OK: return XIAOTAI_OK;
    case JPEG_ERR_NO_MEM: return XIAOTAI_ERR_NO_MEM;
    case JPEG_ERR_INVALID_PARAM: return XIAOTAI_ERR_INVALID_ARG;
    case JPEG_ERR_UNSUPPORT_FMT:
    case JPEG_ERR_UNSUPPORT_STD: return XIAOTAI_ERR_NOT_SUPPORTED;
    default: return XIAOTAI_FAIL;
    }
}

static uint32_t round_edge(uint32_t n)
{
    return (n + JPEG_EDGE_ALIGNMENT - 1U) & ~(uint32_t)(JPEG_EDGE_ALIGNMENT - 1U);
}

/* Any hardware decoder that takes the picture is the better one. */
static int match(const xiaotai_video_decoder_config_t *c)
{
    return c && c->codec == XIAOTAI_VIDEO_CODEC_JPEG && !c->max_width == !c->max_height ? 10 : 0;
}

static xiaotai_err_t open_decoder(const xiaotai_video_decoder_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!match(c)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!c->max_width) return XIAOTAI_ERR_INVALID_SIZE;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t handle = NULL;
    const jpeg_error_t err = jpeg_dec_open(&config, &handle);
    if (err != JPEG_ERR_OK) return jpeg_result(err);
    if (!handle) return XIAOTAI_FAIL;
    *state = handle;
    return XIAOTAI_OK;
}

static void close_decoder(void *state)
{
    if (state) (void)jpeg_dec_close(state);
}

static xiaotai_err_t decode(void *state, const xiaotai_video_decoder_config_t *limits,
                           const xiaotai_video_packet_t *packet, uint8_t *out,
                           size_t capacity, xiaotai_video_image_t *image)
{
    if (!image) return XIAOTAI_ERR_INVALID_ARG;
    *image = (xiaotai_video_image_t){0};
    if (!state || !limits || !limits->max_width || !limits->max_height || !packet ||
        packet->codec != XIAOTAI_VIDEO_CODEC_JPEG || !packet->data || !out) return XIAOTAI_ERR_INVALID_ARG;
    if (!packet->size || packet->size > INT_MAX || (uintptr_t)out % JPEG_ALIGNMENT) return XIAOTAI_ERR_INVALID_SIZE;

    jpeg_dec_io_t io = {.inbuf = (uint8_t *)packet->data, .inbuf_len = (int)packet->size};
    jpeg_dec_header_info_t info = {0};
    jpeg_error_t err = jpeg_dec_parse_header(state, &io, &info);
    if (err != JPEG_ERR_OK) return jpeg_result(err);
    const uint32_t max_edge = limits->max_width > limits->max_height ? limits->max_width : limits->max_height;
    if (!info.width || !info.height || info.width > max_edge || info.height > max_edge ||
        round_edge(info.width) * round_edge(info.height) > (uint32_t)limits->max_width * limits->max_height)
        return XIAOTAI_ERR_INVALID_SIZE;
    const size_t bytes = (size_t)info.width * info.height * 2U;
    int needed = 0;
    err = jpeg_dec_get_outbuf_len(state, &needed);
    if (err != JPEG_ERR_OK) return jpeg_result(err);
    if (needed < 0 || (size_t)needed != bytes || bytes > capacity) return XIAOTAI_ERR_INVALID_SIZE;

    io.outbuf = out;
    err = jpeg_dec_process(state, &io);
    if (err != JPEG_ERR_OK) return jpeg_result(err);
    *image = (xiaotai_video_image_t){.pixel = XIAOTAI_VIDEO_PIXEL_RGB565, .data = out, .size = bytes,
        .width = info.width, .height = info.height, .stride = info.width, .rows = info.height, .full_range = true};
    return XIAOTAI_OK;
}

const xiaotai_video_decoder_t video_esp_jpeg_software_decoder = {
    .name = "esp_jpeg_sw", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE,
    .output = XIAOTAI_VIDEO_PIXEL_RGB565, .output_alignment = JPEG_ALIGNMENT,
    .stack_bytes = 16U * 1024U, .stack_internal = false,
    .match = match, .open = open_decoder, .close = close_decoder, .decode_configured = decode,
};
