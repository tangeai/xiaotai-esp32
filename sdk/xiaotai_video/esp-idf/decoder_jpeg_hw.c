// SPDX-License-Identifier: MIT
#include "driver/jpeg_decode.h"
#include "sdkconfig.h"
#include "xiaotai/port/log.h"
#include "xiaotai/video_backend.h"

enum {
    JPEG_EDGE_ALIGNMENT = 16,
    JPEG_DMA_EDGE_MAX = 0x3fff,  /* height and width in the DMA2D descriptor */
    JPEG_TIMEOUT_MS = 100,
    JPEG_CACHE_ALIGNMENT = CONFIG_CACHE_L1_CACHE_LINE_SIZE > CONFIG_CACHE_L2_CACHE_LINE_SIZE
                           ? CONFIG_CACHE_L1_CACHE_LINE_SIZE : CONFIG_CACHE_L2_CACHE_LINE_SIZE,
};
_Static_assert(JPEG_CACHE_ALIGNMENT > 0 && !(JPEG_CACHE_ALIGNMENT & (JPEG_CACHE_ALIGNMENT - 1)),
               "invalid JPEG cache alignment");

static xiaotai_err_t jpeg_error(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return XIAOTAI_OK;
    case ESP_ERR_NO_MEM: return XIAOTAI_ERR_NO_MEM;
    case ESP_ERR_INVALID_ARG: return XIAOTAI_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return XIAOTAI_ERR_INVALID_STATE;
    case ESP_ERR_INVALID_SIZE: return XIAOTAI_ERR_INVALID_SIZE;
    case ESP_ERR_NOT_SUPPORTED: return XIAOTAI_ERR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return XIAOTAI_ERR_TIMEOUT;
    default: return XIAOTAI_FAIL;
    }
}

static uint32_t round_edge(uint32_t n)
{
    return (n + JPEG_EDGE_ALIGNMENT - 1U) & ~(JPEG_EDGE_ALIGNMENT - 1U);
}

static bool limits_valid(const xiaotai_video_decoder_config_t *c)
{
    return c && c->codec == XIAOTAI_VIDEO_CODEC_JPEG && c->max_width && c->max_height &&
           round_edge(c->max_width) <= JPEG_DMA_EDGE_MAX && round_edge(c->max_height) <= JPEG_DMA_EDGE_MAX;
}

static int match(const xiaotai_video_decoder_config_t *c)
{
    const bool probe = c && c->codec == XIAOTAI_VIDEO_CODEC_JPEG && !c->max_width && !c->max_height;
    return (probe || limits_valid(c)) ? 100 : 0;
}

static void close_decoder(void *state)
{
    if (!state) return;
    const esp_err_t err = jpeg_del_decoder_engine(state);
    if (err != ESP_OK) XIAOTAI_LOGW("video_jpeg", "decoder release failed: 0x%x", (unsigned)err);
}

static xiaotai_err_t open_decoder(const xiaotai_video_decoder_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (match(c) <= 0) return XIAOTAI_ERR_NOT_SUPPORTED;
    const jpeg_decode_engine_cfg_t config = {.intr_priority = 0, .timeout_ms = JPEG_TIMEOUT_MS};
    jpeg_decoder_handle_t handle = NULL;
    const esp_err_t err = jpeg_new_decoder_engine(&config, &handle);
    if (err != ESP_OK) {
        close_decoder(handle);
        return jpeg_error(err);
    }
    if (!handle) return XIAOTAI_FAIL;
    *state = handle;
    return XIAOTAI_OK;
}

static xiaotai_err_t decode(void *state, const xiaotai_video_decoder_config_t *limits,
                           const xiaotai_video_packet_t *packet, uint8_t *out,
                           size_t capacity, xiaotai_video_image_t *image)
{
    if (!image) return XIAOTAI_ERR_INVALID_ARG;
    *image = (xiaotai_video_image_t){0};
    if (!state || !limits_valid(limits) || !packet || packet->codec != XIAOTAI_VIDEO_CODEC_JPEG ||
        !packet->data || !out) return XIAOTAI_ERR_INVALID_ARG;
    if (!packet->size || packet->size > UINT32_MAX || (uintptr_t)out % JPEG_CACHE_ALIGNMENT)
        return XIAOTAI_ERR_INVALID_SIZE;

    jpeg_decode_picture_info_t pic = {0};
    esp_err_t err = jpeg_decoder_get_info(packet->data, (uint32_t)packet->size, &pic);
    if (err != ESP_OK) return jpeg_error(err);
    const uint32_t max_edge = limits->max_width > limits->max_height ? limits->max_width : limits->max_height;
    if (!pic.width || !pic.height || pic.width > max_edge || pic.height > max_edge)
        return XIAOTAI_ERR_INVALID_SIZE;
    const uint32_t stride = round_edge(pic.width), rows = round_edge(pic.height);
    const uint32_t pixels = stride * rows;
    const uint32_t bytes = pixels * 2U;
    if (pixels > (uint32_t)limits->max_width * limits->max_height || bytes > capacity ||
        bytes % JPEG_CACHE_ALIGNMENT) return XIAOTAI_ERR_INVALID_SIZE;

    /* The old renderer made the JPEG engine produce panel-order RGB565. */
    const jpeg_decode_cfg_t config = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB, .conv_std = JPEG_YUV_RGB_CONV_STD_BT601};
    uint32_t written = 0;
    err = jpeg_decoder_process(state, &config, packet->data, (uint32_t)packet->size, out, bytes, &written);
    if (err != ESP_OK) return jpeg_error(err);
    if (written != bytes) return XIAOTAI_ERR_INVALID_SIZE;
    *image = (xiaotai_video_image_t){.pixel = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .data = out, .size = bytes,
        .width = (uint16_t)pic.width, .height = (uint16_t)pic.height, .stride = (uint16_t)stride,
        .rows = (uint16_t)rows, .full_range = true};
    return XIAOTAI_OK;
}

const xiaotai_video_decoder_t video_esp_jpeg_decoder = {
    .name = "esp_jpeg_hw", .load = XIAOTAI_VIDEO_LOAD_HARDWARE,
    .flags = XIAOTAI_VIDEO_DECODER_PERSISTENT,
    .output = XIAOTAI_VIDEO_PIXEL_RGB565_SWAPPED, .output_alignment = JPEG_CACHE_ALIGNMENT,
    .stack_bytes = 8U * 1024U, .stack_internal = true,
    .match = match, .open = open_decoder, .close = close_decoder, .decode_configured = decode,
};
