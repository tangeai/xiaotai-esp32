// SPDX-License-Identifier: MIT
#include "esp_cache.h"
#include "esp_h264_enc_single_hw.h"
#include "h264_au.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/video_backend.h"

enum {
    MB_SIZE = 16, MIN_DIMENSION = 80, MAX_WIDTH = 1920, MAX_HEIGHT = 2032,
    INTERNAL_ALIGNMENT = 64, OUTPUT_ALIGNMENT = 128,
    DB_TMP_BYTES_PER_MB = 128, DB_TMP_GUARD = 79,
};

typedef struct {
    esp_h264_enc_handle_t handle;
    esp_h264_enc_param_hw_handle_t param;
    xiaotai_video_encoder_config_t config;
    bool opened, usable;
} hw_encoder_t;

static xiaotai_err_t result(esp_h264_err_t err)
{
    switch (err) {
        case ESP_H264_ERR_OK: return XIAOTAI_OK;
        case ESP_H264_ERR_ARG: return XIAOTAI_ERR_INVALID_ARG;
        case ESP_H264_ERR_MEM: return XIAOTAI_ERR_NO_MEM;
        case ESP_H264_ERR_TIMEOUT: return XIAOTAI_ERR_TIMEOUT;
        case ESP_H264_ERR_OVERFLOW: return XIAOTAI_ERR_INVALID_SIZE;
        case ESP_H264_ERR_UNSUPPORTED: return XIAOTAI_ERR_NOT_SUPPORTED;
        default: return XIAOTAI_FAIL;
    }
}

static size_t rounded(size_t bytes, size_t alignment)
{
    return (bytes + alignment - 1U) & ~(alignment - 1U);
}

static bool geometry(const xiaotai_video_encoder_config_t *c)
{
    /* Paired 4:2:0 pictures are tight and can carry bottom padding only. */
    return c && c->codec == XIAOTAI_VIDEO_CODEC_H264 && c->pixel == XIAOTAI_VIDEO_PIXEL_YUV420_PAIRED &&
           c->width >= MIN_DIMENSION && c->width <= MAX_WIDTH && c->width % MB_SIZE == 0U &&
           c->height >= MIN_DIMENSION && c->height <= MAX_HEIGHT && !(c->height & 1U) &&
           c->rows == rounded(c->height, MB_SIZE);
}

static bool rates(const xiaotai_video_encoder_config_t *c)
{
    return c->fps && c->bitrate_bps && c->key_interval_frames && c->key_interval_frames <= UINT8_MAX &&
           c->min_qp <= c->max_qp && c->max_qp <= ESP_H264_QP_MAX;
}

static int match(const xiaotai_video_encoder_config_t *c)
{
    return geometry(c) && rates(c) && xiaotai_mem_has_external() ? 100 : 0;
}

static size_t internal_bytes(const xiaotai_video_encoder_config_t *c)
{
    /* The locked hardware library stores reference frames in external RAM.
     * Its largest mandatory internal block is the deblock temporary buffer. */
    return geometry(c) ? rounded(c->width / MB_SIZE * DB_TMP_BYTES_PER_MB + DB_TMP_GUARD,
                                 INTERNAL_ALIGNMENT) : 0;
}

static void close_hw(void *state)
{
    hw_encoder_t *enc = state;
    if (!enc) return;
    if (enc->opened) (void)esp_h264_enc_close(enc->handle);
    if (enc->handle) (void)esp_h264_enc_del(enc->handle);
    xiaotai_mem_free(enc);
}

static xiaotai_err_t open_hw(const xiaotai_video_encoder_config_t *c, void **state)
{
    if (!state) return XIAOTAI_ERR_INVALID_ARG;
    *state = NULL;
    if (!geometry(c) || !xiaotai_mem_has_external()) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (!rates(c)) return XIAOTAI_ERR_INVALID_ARG;
    if (!c->output || (uintptr_t)c->output % OUTPUT_ALIGNMENT ||
        c->output_capacity < OUTPUT_ALIGNMENT || c->output_capacity > UINT32_MAX ||
        c->output_capacity % OUTPUT_ALIGNMENT || !xiaotai_mem_is_external(c->output))
        return XIAOTAI_ERR_INVALID_SIZE;

    hw_encoder_t *enc = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 1, sizeof(*enc));
    if (!enc) return XIAOTAI_ERR_NO_MEM;
    esp_h264_enc_cfg_hw_t cfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY, .gop = (uint8_t)c->key_interval_frames, .fps = c->fps,
        .res = {.width = c->width, .height = c->height},
        .rc = {.bitrate = c->bitrate_bps, .qp_min = c->min_qp, .qp_max = c->max_qp},
    };
    if (c->full_range) {
        cfg.vui_color = (esp_h264_enc_vui_color_t){
            .present = true, .full_range = true, .color_description_present = true,
            .color_primaries = 2, .transfer_characteristics = 2, .matrix_coefficients = 6,
        };
    }
    xiaotai_err_t err = result(esp_h264_enc_hw_new(&cfg, &enc->handle));
    if (err == XIAOTAI_OK && !enc->handle) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK) err = result(esp_h264_enc_hw_get_param_hd(enc->handle, &enc->param));
    if (err == XIAOTAI_OK && !enc->param) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK) err = result(esp_h264_enc_open(enc->handle));
    if (err != XIAOTAI_OK) {
        close_hw(enc);
        return err;
    }
    enc->opened = enc->usable = true;
    enc->config = *c;
    *state = enc;
    return XIAOTAI_OK;
}

static xiaotai_err_t reconfigure(void *state, const xiaotai_video_encoder_config_t *c)
{
    hw_encoder_t *enc = state;
    if (!enc || !c || !rates(c)) return XIAOTAI_ERR_INVALID_ARG;
    if (!enc->usable) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_video_encoder_config_t *old = &enc->config;
    if (c->codec != old->codec || c->pixel != old->pixel || c->width != old->width ||
        c->height != old->height || c->rows != old->rows || c->full_range != old->full_range ||
        c->output != old->output || c->output_capacity != old->output_capacity ||
        c->min_qp != old->min_qp || c->max_qp != old->max_qp) return XIAOTAI_ERR_NOT_SUPPORTED;

    esp_h264_enc_param_handle_t param = &enc->param->base;
    xiaotai_err_t err = XIAOTAI_OK;
    if (c->bitrate_bps != old->bitrate_bps) err = result(esp_h264_enc_set_bitrate(param, c->bitrate_bps));
    if (err == XIAOTAI_OK && c->fps != old->fps) err = result(esp_h264_enc_set_fps(param, c->fps));
    if (err == XIAOTAI_OK && c->key_interval_frames != old->key_interval_frames)
        err = result(esp_h264_enc_set_gop(param, (uint8_t)c->key_interval_frames));
    if (err == XIAOTAI_OK) enc->config = *c;
    else enc->usable = false; /* A sequence of vendor setters is not atomic. */
    return err;
}

static xiaotai_err_t encode(void *state, const xiaotai_video_image_t *image, bool force_key,
                            xiaotai_video_packet_t *packet)
{
    hw_encoder_t *enc = state;
    if (!packet) return XIAOTAI_ERR_INVALID_ARG;
    *packet = (xiaotai_video_packet_t){0};
    if (!enc || !image) return XIAOTAI_ERR_INVALID_ARG;
    if (!enc->usable) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_video_encoder_config_t *c = &enc->config;
    const size_t bytes = (size_t)c->width * c->rows * 3U / 2U;
    if (!image->data || image->width != c->width || image->height != c->height || image->rows != c->rows ||
        image->pixel != c->pixel || image->full_range != c->full_range ||
        (image->stride && image->stride != c->width) || image->size < bytes ||
        (uintptr_t)image->data % MB_SIZE || !xiaotai_mem_is_external(image->data)) return XIAOTAI_ERR_INVALID_SIZE;

    xiaotai_err_t err = esp_cache_msync((void *)image->data, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (err != XIAOTAI_OK) return err;
    if (force_key) {
        err = result(esp_h264_enc_force_idr(&enc->param->base));
        if (err != XIAOTAI_OK) return err;
    }
    const uint32_t stamp = image->captured_us > 0 ? (uint32_t)(image->captured_us / 1000) : 0;
    esp_h264_enc_in_frame_t input = {.raw_data = {.buffer = (uint8_t *)image->data, .len = (uint32_t)bytes}, .pts = stamp};
    esp_h264_enc_out_frame_t output = {.raw_data = {.buffer = c->output, .len = (uint32_t)c->output_capacity}};
    err = result(esp_h264_enc_process(enc->handle, &input, &output));
    if (err != XIAOTAI_OK) return err;
    if (!output.length || output.length > c->output_capacity) return XIAOTAI_ERR_INVALID_SIZE;
    err = esp_cache_msync(c->output, rounded(output.length, OUTPUT_ALIGNMENT),
                          ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    if (err != XIAOTAI_OK) return err;

    const video_h264_au_t au = video_h264_inspect(c->output, output.length);
    const bool key = au.idr && au.sps && au.pps;
    if (!au.valid || (au.idr && !key) || (force_key && !key)) return XIAOTAI_FAIL;
    *packet = (xiaotai_video_packet_t){.codec = c->codec, .key_frame = key, .width = c->width, .height = c->height,
        .timestamp_ms = stamp, .data = c->output, .size = output.length};
    return XIAOTAI_OK;
}

const xiaotai_video_encoder_t video_esp_h264_encoder = {
    .name = "h264.hw", .load = XIAOTAI_VIDEO_LOAD_HARDWARE, .output_alignment = OUTPUT_ALIGNMENT,
    .internal_bytes = internal_bytes, .match = match, .open = open_hw, .reconfigure = reconfigure,
    .encode = encode, .close = close_hw,
};
