// SPDX-License-Identifier: MIT
#include <limits.h>
#include "esp_jpeg_enc.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/video_backend.h"

enum { JPEG_ALIGNMENT = 16 };

typedef struct {
    jpeg_enc_handle_t handle;
    xiaotai_video_encoder_config_t config;
    int input_size;
} jpeg_encoder_t;

static xiaotai_err_t jpeg_result(jpeg_error_t result)
{
    switch (result) {
    case JPEG_ERR_OK: return XIAOTAI_OK;
    case JPEG_ERR_NO_MEM: return XIAOTAI_ERR_NO_MEM;
    case JPEG_ERR_NO_MORE_DATA: return XIAOTAI_ERR_INVALID_SIZE;
    case JPEG_ERR_INVALID_PARAM: return XIAOTAI_ERR_INVALID_ARG;
    case JPEG_ERR_UNSUPPORT_FMT:
    case JPEG_ERR_UNSUPPORT_STD: return XIAOTAI_ERR_NOT_SUPPORTED;
    default: return XIAOTAI_FAIL;
    }
}

static int match(const xiaotai_video_encoder_config_t *config)
{
    return config && config->codec == XIAOTAI_VIDEO_CODEC_JPEG && config->pixel == XIAOTAI_VIDEO_PIXEL_YUYV422 &&
           config->width && config->height && !((config->width | config->height) & 1U) &&
           config->rows == config->height && (uint64_t)config->width * config->height * 2U <= INT_MAX &&
           xiaotai_mem_has_external();
}

static bool aligned(const void *buffer)
{
    return buffer && (uintptr_t)buffer % JPEG_ALIGNMENT == 0U;
}

static xiaotai_err_t open(const xiaotai_video_encoder_config_t *config, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!match(config)) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!config->quality || config->quality > 100U || !aligned(config->output)) return XIAOTAI_ERR_INVALID_ARG;
    if (config->output_capacity < JPEG_ALIGNMENT || config->output_capacity > INT_MAX ||
        config->output_capacity % JPEG_ALIGNMENT) return XIAOTAI_ERR_INVALID_SIZE;

    jpeg_encoder_t *encoder = xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 1, sizeof(*encoder));
    if (!encoder) return XIAOTAI_ERR_NO_MEM;
    jpeg_enc_config_t vendor = DEFAULT_JPEG_ENC_CONFIG();
    vendor.width = config->width;
    vendor.height = config->height;
    vendor.src_type = JPEG_PIXEL_FORMAT_YCbYCr;
    vendor.subsampling = JPEG_SUBSAMPLE_420;
    vendor.quality = config->quality;
    vendor.task_enable = false;
    const jpeg_error_t native = jpeg_enc_open(&vendor, &encoder->handle);
    const xiaotai_err_t result = jpeg_result(native);
    if (result != XIAOTAI_OK || !encoder->handle) {
        XIAOTAI_LOGE("video", "encoder open failed width=%u height=%u rc=%d handle=%u",
                     (unsigned)config->width, (unsigned)config->height, (int)native, (unsigned)(encoder->handle != NULL));
        xiaotai_mem_free(encoder);
        return result != XIAOTAI_OK ? result : XIAOTAI_FAIL;
    }
    encoder->config = *config;
    encoder->input_size = (int)((uint64_t)config->width * config->height * 2U);
    *state = encoder;
    return XIAOTAI_OK;
}

static xiaotai_err_t reconfigure(void *state, const xiaotai_video_encoder_config_t *config)
{
    jpeg_encoder_t *encoder = state;
    if (!encoder || !config || !config->quality || config->quality > 100U) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_video_encoder_config_t *old = &encoder->config;
    if (config->codec != old->codec || config->pixel != old->pixel || config->width != old->width ||
        config->height != old->height || config->rows != old->rows || config->full_range != old->full_range ||
        config->output != old->output || config->output_capacity != old->output_capacity) {
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    if (config->quality != old->quality) {
        const xiaotai_err_t result = jpeg_result(jpeg_enc_set_quality(encoder->handle, config->quality));
        if (result != XIAOTAI_OK) return result;
    }
    encoder->config = *config;
    return XIAOTAI_OK;
}

static xiaotai_err_t encode(void *state, const xiaotai_video_image_t *image, bool force_key,
                            xiaotai_video_packet_t *packet)
{
    (void)force_key; /* JPEG frames are independently decodable. */
    const jpeg_encoder_t *encoder = state;
    if (!packet) return XIAOTAI_ERR_INVALID_ARG;
    *packet = (xiaotai_video_packet_t){0};
    if (!encoder || !image) return XIAOTAI_ERR_INVALID_ARG;
    const xiaotai_video_encoder_config_t *config = &encoder->config;
    if (image->pixel != config->pixel || image->width != config->width || image->height != config->height ||
        image->rows != config->rows || (image->stride && image->stride != image->width) ||
        image->size != (size_t)encoder->input_size || !aligned(image->data) ||
        !xiaotai_mem_is_external(image->data)) return XIAOTAI_ERR_INVALID_SIZE;

    int length = 0;
    const xiaotai_err_t result = jpeg_result(jpeg_enc_process(encoder->handle, image->data, encoder->input_size,
        config->output, (int)config->output_capacity, &length));
    if (result != XIAOTAI_OK) return result;
    if (length < 4 || (size_t)length > config->output_capacity) return XIAOTAI_ERR_INVALID_SIZE;
    if (config->output[0] != 0xff || config->output[1] != 0xd8 ||
        config->output[length - 2] != 0xff || config->output[length - 1] != 0xd9) return XIAOTAI_FAIL;

    *packet = (xiaotai_video_packet_t){.codec = config->codec, .key_frame = true, .width = config->width,
        .height = config->height, .timestamp_ms = (uint32_t)(image->captured_us / 1000),
        .data = config->output, .size = (size_t)length};
    return XIAOTAI_OK;
}

static void close(void *state)
{
    jpeg_encoder_t *encoder = state;
    if (!encoder) return;
    (void)jpeg_enc_close(encoder->handle);
    xiaotai_mem_free(encoder);
}

const xiaotai_video_encoder_t video_esp_jpeg_encoder = {
    .name = "jpeg_esp_sw", .load = XIAOTAI_VIDEO_LOAD_SOFTWARE, .output_alignment = JPEG_ALIGNMENT,
    .match = match, .open = open, .reconfigure = reconfigure, .encode = encode, .close = close,
};
