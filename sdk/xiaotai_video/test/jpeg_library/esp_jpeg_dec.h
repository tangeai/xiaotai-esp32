// SPDX-License-Identifier: MIT
/* The calls of the JPEG library that decoder_jpeg_sw.c makes, for the host
 * test: the library is a target archive. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    JPEG_ERR_OK = 0, JPEG_ERR_FAIL = -1, JPEG_ERR_NO_MEM = -2, JPEG_ERR_NO_MORE_DATA = -3,
    JPEG_ERR_INVALID_PARAM = -4, JPEG_ERR_BAD_DATA = -5, JPEG_ERR_UNSUPPORT_FMT = -6, JPEG_ERR_UNSUPPORT_STD = -7,
} jpeg_error_t;
typedef enum { JPEG_PIXEL_FORMAT_RGB888 = 1, JPEG_PIXEL_FORMAT_RGB565_LE = 2 } jpeg_pixel_format_t;
typedef void *jpeg_dec_handle_t;
typedef struct { jpeg_pixel_format_t output_type; bool block_enable; } jpeg_dec_config_t;
typedef struct { uint16_t width, height; } jpeg_dec_header_info_t;
typedef struct {
    uint8_t *inbuf;
    int inbuf_len, inbuf_remain;
    uint8_t *outbuf;
    int out_size;
} jpeg_dec_io_t;

#define DEFAULT_JPEG_DEC_CONFIG() {.output_type = JPEG_PIXEL_FORMAT_RGB888, .block_enable = false}

jpeg_error_t jpeg_dec_open(jpeg_dec_config_t *config, jpeg_dec_handle_t *handle);
jpeg_error_t jpeg_dec_parse_header(jpeg_dec_handle_t handle, jpeg_dec_io_t *io, jpeg_dec_header_info_t *info);
jpeg_error_t jpeg_dec_get_outbuf_len(jpeg_dec_handle_t handle, int *length);
jpeg_error_t jpeg_dec_process(jpeg_dec_handle_t handle, jpeg_dec_io_t *io);
jpeg_error_t jpeg_dec_close(jpeg_dec_handle_t handle);
