// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdalign.h>
#include <string.h>
#include "esp_jpeg_dec.h"
#include "xiaotai/video_backend.h"

extern const xiaotai_video_decoder_t video_esp_jpeg_software_decoder;
static const xiaotai_video_decoder_t *const jpeg = &video_esp_jpeg_software_decoder;

static int handle;
static unsigned opens, closes, processed;
static jpeg_error_t open_result, parse_result, process_result;
static bool no_handle, padded;
static jpeg_dec_header_info_t picture = {.width = 10, .height = 6};

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config, jpeg_dec_handle_t *out)
{
    assert(config->output_type == JPEG_PIXEL_FORMAT_RGB565_LE && !config->block_enable);
    ++opens;
    if (open_result == JPEG_ERR_OK && !no_handle) *out = &handle;
    return open_result;
}

jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t state, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info)
{
    assert(state == &handle && io->inbuf_len == 4 && io->inbuf[0] == 0xff && !io->outbuf);
    *info = picture;
    return parse_result;
}

jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t state, int *length)
{
    assert(state == &handle);
    *length = (picture.width + padded) * picture.height * 2;
    return JPEG_ERR_OK;
}

jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t state, jpeg_dec_io_t *io)
{
    assert(state == &handle);
    ++processed;
    memset(io->outbuf, 0x5a, (size_t)picture.width * picture.height * 2U);
    return process_result;
}

jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t state)
{
    assert(state == &handle);
    ++closes;
    return JPEG_ERR_OK;
}

static void open_test(xiaotai_video_decoder_config_t *c, void **state)
{
    assert(jpeg->load == XIAOTAI_VIDEO_LOAD_SOFTWARE && jpeg->output == XIAOTAI_VIDEO_PIXEL_RGB565 && !jpeg->flags);
    const int score = jpeg->match(c);
    assert(score > 0 && score < 100);   /* hardware first */
    *state = c;
    assert(jpeg->open(c, state) == XIAOTAI_ERR_INVALID_SIZE && !*state);
    c->max_width = 64;
    assert(!jpeg->match(c) && jpeg->open(c, state) == XIAOTAI_ERR_NOT_SUPPORTED);
    c->max_height = 32;
    assert(jpeg->match(c) == score);
    xiaotai_video_decoder_config_t h264 = *c;
    h264.codec = XIAOTAI_VIDEO_CODEC_H264;
    assert(!jpeg->match(&h264) && !jpeg->match(NULL) && !opens);
    open_result = JPEG_ERR_NO_MEM;
    assert(jpeg->open(c, state) == XIAOTAI_ERR_NO_MEM && !*state);
    open_result = JPEG_ERR_OK;
    no_handle = true;
    assert(jpeg->open(c, state) == XIAOTAI_FAIL && !*state);
    no_handle = false;
    assert(jpeg->open(c, state) == XIAOTAI_OK && *state == &handle);
}

int main(void)
{
    xiaotai_video_decoder_config_t c = {.codec = XIAOTAI_VIDEO_CODEC_JPEG};
    void *state;
    open_test(&c, &state);

    static const uint8_t data[] = {0xff, 0xd8, 0xff, 0xd9};
    xiaotai_video_packet_t packet = {.codec = XIAOTAI_VIDEO_CODEC_JPEG, .data = data, .size = sizeof(data)};
    alignas(16) static uint8_t out[64 * 32 * 2 + 16];
    const size_t room = 64U * 32U * 2U;
    xiaotai_video_image_t image;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_OK);
    assert(image.pixel == XIAOTAI_VIDEO_PIXEL_RGB565 && image.data == out && image.width == 10 && image.height == 6 &&
           image.stride == 10 && image.rows == 6 && image.size == 120 && image.full_range && out[119] == 0x5a && !out[120]);
    assert(jpeg->decode_configured(state, &c, &packet, out + 8, room, &image) == XIAOTAI_ERR_INVALID_SIZE && !image.data);
    assert(jpeg->decode_configured(state, &c, &packet, out, 119, &image) == XIAOTAI_ERR_INVALID_SIZE);
    padded = true;      /* what it does not describe is not shown */
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_SIZE);
    padded = false;
    assert(processed == 1);

    /* Either way round within the budget, never past the long side. */
    picture = (jpeg_dec_header_info_t){.width = 32, .height = 64};
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_OK && image.height == 64);
    picture = (jpeg_dec_header_info_t){.width = 65, .height = 16};
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_SIZE);
    picture = (jpeg_dec_header_info_t){.width = 33, .height = 64};
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_SIZE);
    picture = (jpeg_dec_header_info_t){0};
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_SIZE && processed == 2);

    picture = (jpeg_dec_header_info_t){.width = 10, .height = 6};
    parse_result = JPEG_ERR_UNSUPPORT_STD;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_NOT_SUPPORTED);
    parse_result = JPEG_ERR_BAD_DATA;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_FAIL);
    parse_result = JPEG_ERR_OK;
    process_result = JPEG_ERR_NO_MEM;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_NO_MEM && !image.data);
    packet.size = 0;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_SIZE);
    packet.data = NULL;
    assert(jpeg->decode_configured(state, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_ARG);
    assert(jpeg->decode_configured(NULL, &c, &packet, out, room, &image) == XIAOTAI_ERR_INVALID_ARG);
    jpeg->close(state);
    jpeg->close(NULL);
    assert(closes == 1);
    return 0;
}
