#include "video_frame_converter.h"

#include <stdlib.h>
#include <string.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"

static const char *TAG = "video_convert";

struct video_frame_converter {
    video_frame_converter_config_t config;
    portMUX_TYPE stats_lock;
    int32_t luma_terms[256];
    ppa_client_handle_t ppa_client;
    uint8_t *packed_yuv420;
    size_t packed_yuv420_size;
    video_frame_converter_stats_t stats;
    bool ppa_timing_logged;
    video_frame_converter_mode_t last_mode;
};

#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define VIDEO_FRAME_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define VIDEO_FRAME_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

#define VIDEO_FRAME_PPA_SCALE_STEPS 16U
#define VIDEO_FRAME_PPA_SCALE_MAX_STEPS 255U

typedef struct {
    float scale;
    uint16_t render_width;
    uint16_t render_height;
    uint16_t offset_x;
    uint16_t offset_y;
} video_frame_ppa_fit_t;

static void video_frame_record_render_layout(
    video_frame_converter_handle_t handle,
    uint16_t render_width,
    uint16_t render_height,
    uint16_t offset_x,
    uint16_t offset_y)
{
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.last_render_width = render_width;
    handle->stats.last_render_height = render_height;
    handle->stats.last_offset_x = offset_x;
    handle->stats.last_offset_y = offset_y;
    taskEXIT_CRITICAL(&handle->stats_lock);
}

static size_t video_frame_align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static inline void video_frame_pack_ouev_row(const uint8_t *luma,
                                             const uint8_t *chroma,
                                             uint32_t width,
                                             uint8_t *output)
{
    uint32_t x = 0;

    /* OUEV stores one chroma byte for each two luma bytes. Pack eight
     * pixels into three aligned words so PSRAM writes stay sequential. */
    for (; x + 8U <= width; x += 8U) {
        uint32_t y0 = 0;
        uint32_t y1 = 0;
        uint32_t c = 0;
        memcpy(&y0, luma + x, sizeof(y0));
        memcpy(&y1, luma + x + 4U, sizeof(y1));
        memcpy(&c, chroma + (x / 2U), sizeof(c));

        uint32_t *words = (uint32_t *)(output + ((size_t)x * 3U / 2U));
        words[0] = (c & 0x000000FFU) |
                   ((y0 & 0x000000FFU) << 8) |
                   ((y0 & 0x0000FF00U) << 8) |
                   ((c & 0x0000FF00U) << 16);
        words[1] = ((y0 >> 16) & 0x000000FFU) |
                   ((y0 >> 16) & 0x0000FF00U) |
                   ((c & 0x00FF0000U)) |
                   ((y1 & 0x000000FFU) << 24);
        words[2] = ((y1 >> 8) & 0x000000FFU) |
                   ((c >> 16) & 0x0000FF00U) |
                   (y1 & 0x00FF0000U) |
                   (y1 & 0xFF000000U);
    }

    for (; x < width; x += 2U) {
        size_t output_offset = (size_t)x * 3U / 2U;
        output[output_offset] = chroma[x / 2U];
        output[output_offset + 1U] = luma[x];
        output[output_offset + 2U] = luma[x + 1U];
    }
}

static void video_frame_pack_i420_region_for_ppa(const uint8_t *i420,
                                                 uint16_t source_width,
                                                 uint16_t source_height,
                                                 uint16_t crop_x,
                                                 uint16_t crop_y,
                                                 uint16_t crop_width,
                                                 uint16_t crop_height,
                                                 uint8_t *output)
{
    const size_t luma_size = (size_t)source_width * source_height;
    const uint8_t *plane_y = i420;
    const uint8_t *plane_u = plane_y + luma_size;
    const uint8_t *plane_v = plane_u + (luma_size / 4U);
    const size_t packed_stride = (size_t)crop_width * 3U / 2U;
    const size_t chroma_stride = source_width / 2U;

    for (uint32_t row = 0; row < crop_height; row += 2U) {
        uint32_t source_y = crop_y + row;
        video_frame_pack_ouev_row(plane_y + ((size_t)source_y * source_width) + crop_x,
                                  plane_u + ((size_t)(source_y / 2U) * chroma_stride) +
                                      (crop_x / 2U),
                                  crop_width,
                                  output + ((size_t)row * packed_stride));
        video_frame_pack_ouev_row(plane_y + ((size_t)(source_y + 1U) * source_width) + crop_x,
                                  plane_v + ((size_t)(source_y / 2U) * chroma_stride) +
                                      (crop_x / 2U),
                                  crop_width,
                                  output + ((size_t)(row + 1U) * packed_stride));
    }
}

static void video_frame_swap_rgb565_bytes(uint16_t *pixels, size_t pixel_count)
{
    uint32_t *words = (uint32_t *)pixels;
    size_t word_count = pixel_count / 2U;

    for (size_t index = 0; index < word_count; ++index) {
        uint32_t value = words[index];
        words[index] = ((value & 0x00FF00FFU) << 8) |
                       ((value & 0xFF00FF00U) >> 8);
    }
    if ((pixel_count & 1U) != 0U) {
        uint16_t value = pixels[pixel_count - 1U];
        pixels[pixel_count - 1U] = (uint16_t)((value >> 8) | (value << 8));
    }
}

static bool video_frame_rotation_is_valid(video_frame_rotation_t rotation)
{
    return rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_0 ||
           rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_90 ||
           rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_180 ||
           rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_270;
}

static bool video_frame_rotation_swaps_axes(video_frame_rotation_t rotation)
{
    return rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_90 ||
           rotation == VIDEO_FRAME_ROTATION_CLOCKWISE_270;
}

static bool video_frame_fit_mode_is_valid(video_frame_fit_mode_t fit_mode)
{
    return fit_mode == VIDEO_FRAME_FIT_CONTAIN ||
           fit_mode == VIDEO_FRAME_FIT_COVER;
}

static ppa_srm_rotation_angle_t video_frame_rotation_to_ppa(
    video_frame_rotation_t rotation)
{
    switch (rotation) {
    case VIDEO_FRAME_ROTATION_CLOCKWISE_90:
        return PPA_SRM_ROTATION_ANGLE_270;
    case VIDEO_FRAME_ROTATION_CLOCKWISE_180:
        return PPA_SRM_ROTATION_ANGLE_180;
    case VIDEO_FRAME_ROTATION_CLOCKWISE_270:
        return PPA_SRM_ROTATION_ANGLE_90;
    case VIDEO_FRAME_ROTATION_CLOCKWISE_0:
    default:
        return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static bool video_frame_resolve_ppa_fit(uint16_t crop_width,
                                        uint16_t crop_height,
                                        uint16_t output_width,
                                        uint16_t output_height,
                                        video_frame_rotation_t rotation,
                                        bool prevent_upscale,
                                        video_frame_ppa_fit_t *fit)
{
    if (crop_width == 0U || crop_height == 0U ||
        output_width == 0U || output_height == 0U ||
        !video_frame_rotation_is_valid(rotation) || fit == NULL) {
        return false;
    }

    uint16_t rotated_width =
        video_frame_rotation_swaps_axes(rotation) ? crop_height : crop_width;
    uint16_t rotated_height =
        video_frame_rotation_swaps_axes(rotation) ? crop_width : crop_height;
    uint32_t scale_x_steps =
        ((uint32_t)output_width * VIDEO_FRAME_PPA_SCALE_STEPS) / rotated_width;
    uint32_t scale_y_steps =
        ((uint32_t)output_height * VIDEO_FRAME_PPA_SCALE_STEPS) / rotated_height;
    uint32_t scale_steps =
        scale_x_steps < scale_y_steps ? scale_x_steps : scale_y_steps;
    if (prevent_upscale && scale_steps > VIDEO_FRAME_PPA_SCALE_STEPS) {
        scale_steps = VIDEO_FRAME_PPA_SCALE_STEPS;
    }
    if (scale_steps == 0U || scale_steps > VIDEO_FRAME_PPA_SCALE_MAX_STEPS) {
        return false;
    }

    uint32_t render_width =
        ((uint32_t)rotated_width * scale_steps) / VIDEO_FRAME_PPA_SCALE_STEPS;
    uint32_t render_height =
        ((uint32_t)rotated_height * scale_steps) / VIDEO_FRAME_PPA_SCALE_STEPS;
    if (render_width == 0U || render_height == 0U ||
        render_width > output_width || render_height > output_height) {
        return false;
    }

    fit->scale = (float)scale_steps / (float)VIDEO_FRAME_PPA_SCALE_STEPS;
    fit->render_width = (uint16_t)render_width;
    fit->render_height = (uint16_t)render_height;
    fit->offset_x = (uint16_t)((output_width - render_width) / 2U);
    fit->offset_y = (uint16_t)((output_height - render_height) / 2U);
    return true;
}

static esp_err_t video_frame_convert_yuv420_ppa(video_frame_converter_handle_t handle,
                                               const uint8_t *yuv420,
                                               uint16_t source_width,
                                               uint16_t source_height,
                                               uint16_t crop_x,
                                               uint16_t crop_y,
                                               uint16_t crop_width,
                                               uint16_t crop_height,
                                               bool input_is_ouyy_evyy,
                                               uint16_t *output)
{
    size_t packed_size = (size_t)crop_width * crop_height * 3U / 2U;
    uint16_t output_width = handle->config.output_width;
    uint16_t output_height = handle->config.output_height;
    size_t output_size = (size_t)output_width * output_height * sizeof(*output);
    video_frame_ppa_fit_t fit = {0};

    ESP_RETURN_ON_FALSE(handle->ppa_client != NULL &&
                            (input_is_ouyy_evyy || handle->packed_yuv420 != NULL) &&
                            video_frame_resolve_ppa_fit(crop_width,
                                                        crop_height,
                                                        output_width,
                                                        output_height,
                                                        VIDEO_FRAME_ROTATION_CLOCKWISE_0,
                                                        handle->config.prevent_upscale,
                                                        &fit) &&
                            (input_is_ouyy_evyy ||
                             packed_size <= handle->packed_yuv420_size) &&
                            ((uintptr_t)output & (VIDEO_FRAME_CACHE_LINE_SIZE - 1U)) == 0U &&
                            (output_size & (VIDEO_FRAME_CACHE_LINE_SIZE - 1U)) == 0U,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "PPA conversion buffer is not compatible");

    video_frame_record_render_layout(handle,
                                     fit.render_width,
                                     fit.render_height,
                                     fit.offset_x,
                                     fit.offset_y);
    int64_t started_us = esp_timer_get_time();
    const uint8_t *ppa_input = yuv420;
    uint16_t ppa_input_width = source_width;
    uint16_t ppa_input_height = source_height;
    uint16_t ppa_crop_x = crop_x;
    uint16_t ppa_crop_y = crop_y;
    if (!input_is_ouyy_evyy) {
        video_frame_pack_i420_region_for_ppa(yuv420,
                                             source_width,
                                             source_height,
                                             crop_x,
                                             crop_y,
                                             crop_width,
                                             crop_height,
                                             handle->packed_yuv420);
        ppa_input = handle->packed_yuv420;
        ppa_input_width = crop_width;
        ppa_input_height = crop_height;
        ppa_crop_x = 0U;
        ppa_crop_y = 0U;
    }
    if (fit.render_width != output_width || fit.render_height != output_height) {
        memset(output, 0, output_size);
        ESP_RETURN_ON_ERROR(esp_cache_msync(output,
                                            output_size,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                            TAG,
                            "sync PPA letterbox background failed");
    }
    int64_t packed_us = esp_timer_get_time();
    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = ppa_input,
            .pic_w = ppa_input_width,
            .pic_h = ppa_input_height,
            .block_w = crop_width,
            .block_h = crop_height,
            .block_offset_x = ppa_crop_x,
            .block_offset_y = ppa_crop_y,
            .srm_cm = PPA_SRM_COLOR_MODE_YUV420,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = output,
            .buffer_size = output_size,
            .pic_w = output_width,
            .pic_h = output_height,
            .block_offset_x = fit.offset_x,
            .block_offset_y = fit.offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = fit.scale,
        .scale_y = fit.scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(handle->ppa_client, &operation),
                        TAG,
                        "PPA YUV420 conversion failed");
    int64_t ppa_done_us = esp_timer_get_time();
    if (handle->config.output_rgb565_byte_swap) {
        video_frame_swap_rgb565_bytes(output, (size_t)output_width * output_height);
    }
    int64_t done_us = esp_timer_get_time();
    uint32_t pack_us = (uint32_t)(packed_us - started_us);
    uint32_t ppa_us = (uint32_t)(ppa_done_us - packed_us);
    uint32_t swap_us = (uint32_t)(done_us - ppa_done_us);
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.ppa_frames++;
    handle->stats.pack_time_us += pack_us;
    handle->stats.ppa_time_us += ppa_us;
    handle->stats.swap_time_us += swap_us;
    if (pack_us > handle->stats.pack_max_us) {
        handle->stats.pack_max_us = pack_us;
    }
    if (ppa_us > handle->stats.ppa_max_us) {
        handle->stats.ppa_max_us = ppa_us;
    }
    if (swap_us > handle->stats.swap_max_us) {
        handle->stats.swap_max_us = swap_us;
    }
    taskEXIT_CRITICAL(&handle->stats_lock);
    if (!handle->ppa_timing_logged) {
        handle->ppa_timing_logged = true;
        ESP_LOGI(TAG,
                 "PPA conversion first timing: pack=%lldus hardware_wait=%lldus swap=%lldus total=%lldus",
                 (long long)(packed_us - started_us),
                 (long long)(ppa_done_us - packed_us),
                 (long long)(done_us - ppa_done_us),
                 (long long)(done_us - started_us));
    }
    return ESP_OK;
}

static uint8_t video_frame_clip_u8(int value)
{
    if (value < 0) {
        return 0;
    }
    if (value > 255) {
        return 255;
    }
    return (uint8_t)value;
}

static inline uint16_t video_frame_yuv_terms_to_rgb565(int luma,
                                                       int red_chroma,
                                                       int green_chroma,
                                                       int blue_chroma,
                                                       bool byte_swap)
{
    uint8_t red = video_frame_clip_u8((luma + red_chroma) >> 8);
    uint8_t green = video_frame_clip_u8((luma + green_chroma) >> 8);
    uint8_t blue = video_frame_clip_u8((luma + blue_chroma) >> 8);
    uint16_t value = (uint16_t)(((uint16_t)(red & 0xF8U) << 8) |
                                ((uint16_t)(green & 0xFCU) << 3) |
                                ((uint16_t)blue >> 3));
    return byte_swap ? (uint16_t)((value >> 8) | (value << 8)) : value;
}

static inline uint16_t video_frame_yuv_to_rgb565(const int32_t *luma_terms,
                                                 int y,
                                                 int chroma_u,
                                                 int chroma_v,
                                                 bool byte_swap)
{
    return video_frame_yuv_terms_to_rgb565(luma_terms[y],
                                           409 * chroma_v + 128,
                                           -100 * chroma_u - 208 * chroma_v + 128,
                                           516 * chroma_u + 128,
                                           byte_swap);
}

static void video_frame_fit_inside(uint16_t source_width,
                                   uint16_t source_height,
                                   uint16_t output_width,
                                   uint16_t output_height,
                                   bool prevent_upscale,
                                   uint16_t *render_width,
                                   uint16_t *render_height,
                                   uint16_t *offset_x,
                                   uint16_t *offset_y)
{
    uint32_t width = output_width;
    uint32_t height = output_height;

    if ((uint32_t)source_width * output_height > (uint32_t)source_height * output_width) {
        height = ((uint32_t)source_height * output_width) / source_width;
        height &= ~1U;
    } else {
        width = ((uint32_t)source_width * output_height) / source_height;
        width &= ~1U;
    }

    if (prevent_upscale &&
        (width > source_width || height > source_height)) {
        width = source_width;
        height = source_height;
    }

    *render_width = (uint16_t)width;
    *render_height = (uint16_t)height;
    *offset_x = (uint16_t)((output_width - width) / 2U);
    *offset_y = (uint16_t)((output_height - height) / 2U);
}

static esp_err_t video_frame_convert_i420_region(video_frame_converter_handle_t handle,
                                                 const uint8_t *i420,
                                                 uint16_t source_width,
                                                 uint16_t source_height,
                                                 uint16_t crop_x,
                                                 uint16_t crop_y,
                                                 uint16_t crop_width,
                                                 uint16_t crop_height,
                                                 uint16_t *output)
{
    const size_t luma_size = (size_t)source_width * source_height;
    const uint8_t *plane_y = i420;
    const uint8_t *plane_u = plane_y + luma_size;
    const uint8_t *plane_v = plane_u + (luma_size / 4U);
    const uint32_t chroma_stride = source_width / 2U;

    for (uint32_t row = 0; row < crop_height; row += 2U) {
        uint32_t source_y = crop_y + row;
        const uint8_t *luma_row_0 =
            plane_y + ((size_t)source_y * source_width) + crop_x;
        const uint8_t *luma_row_1 = luma_row_0 + source_width;
        const uint8_t *chroma_u_row =
            plane_u + ((size_t)(source_y / 2U) * chroma_stride) + (crop_x / 2U);
        const uint8_t *chroma_v_row =
            plane_v + ((size_t)(source_y / 2U) * chroma_stride) + (crop_x / 2U);
        uint16_t *output_row_0 = output + ((size_t)row * crop_width);
        uint16_t *output_row_1 = output_row_0 + crop_width;

        for (uint32_t x = 0; x < crop_width; x += 2U) {
            int chroma_u = chroma_u_row[x / 2U] - 128;
            int chroma_v = chroma_v_row[x / 2U] - 128;
            int red_chroma = 409 * chroma_v + 128;
            int green_chroma = -100 * chroma_u - 208 * chroma_v + 128;
            int blue_chroma = 516 * chroma_u + 128;
            bool byte_swap = handle->config.output_rgb565_byte_swap;

            output_row_0[x] = video_frame_yuv_terms_to_rgb565(
                handle->luma_terms[luma_row_0[x]], red_chroma, green_chroma, blue_chroma, byte_swap);
            output_row_0[x + 1U] = video_frame_yuv_terms_to_rgb565(
                handle->luma_terms[luma_row_0[x + 1U]], red_chroma, green_chroma, blue_chroma, byte_swap);
            output_row_1[x] = video_frame_yuv_terms_to_rgb565(
                handle->luma_terms[luma_row_1[x]], red_chroma, green_chroma, blue_chroma, byte_swap);
            output_row_1[x + 1U] = video_frame_yuv_terms_to_rgb565(
                handle->luma_terms[luma_row_1[x + 1U]], red_chroma, green_chroma, blue_chroma, byte_swap);
        }
    }
    return ESP_OK;
}

static esp_err_t video_frame_resolve_crop(video_frame_converter_handle_t handle,
                                          uint16_t source_width,
                                          uint16_t source_height,
                                          uint16_t *crop_x,
                                          uint16_t *crop_y,
                                          uint16_t *crop_width,
                                          uint16_t *crop_height)
{
    *crop_x = handle->config.source_crop_x;
    *crop_y = handle->config.source_crop_y;
    *crop_width = handle->config.source_crop_width != 0U ?
                      handle->config.source_crop_width : source_width;
    *crop_height = handle->config.source_crop_height != 0U ?
                       handle->config.source_crop_height : source_height;

    ESP_RETURN_ON_FALSE(((*crop_x | *crop_y | *crop_width | *crop_height) & 1U) == 0U &&
                            *crop_width >= 16U && *crop_height >= 16U &&
                            (uint32_t)*crop_x + *crop_width <= source_width &&
                            (uint32_t)*crop_y + *crop_height <= source_height,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "invalid source crop");
    return ESP_OK;
}

static esp_err_t video_frame_apply_fit_crop(video_frame_converter_handle_t handle,
                                            video_frame_rotation_t rotation,
                                            bool require_even_alignment,
                                            uint16_t source_width,
                                            uint16_t source_height,
                                            uint16_t *crop_x,
                                            uint16_t *crop_y,
                                            uint16_t *crop_width,
                                            uint16_t *crop_height)
{
    /* PPA requires even coordinates for I420, while RGB565 can keep the exact
     * geometric center when the two source margins are odd-sized. */
    if (handle->config.fit_mode == VIDEO_FRAME_FIT_COVER) {
        const bool swap_axes = video_frame_rotation_swaps_axes(rotation);
        const uint32_t target_width =
            swap_axes ? handle->config.output_height : handle->config.output_width;
        const uint32_t target_height =
            swap_axes ? handle->config.output_width : handle->config.output_height;
        const uint32_t source_ratio =
            (uint32_t)(*crop_width) * target_height;
        const uint32_t target_ratio =
            (uint32_t)(*crop_height) * target_width;

        if (source_ratio > target_ratio) {
            uint16_t fitted_width =
                (uint16_t)(((uint32_t)(*crop_height) * target_width) /
                           target_height);
            fitted_width &= (uint16_t)~1U;
            ESP_RETURN_ON_FALSE(fitted_width >= 16U,
                                ESP_ERR_INVALID_SIZE,
                                TAG,
                                "cover crop width is too small");
            uint16_t offset =
                (uint16_t)((*crop_width - fitted_width) / 2U);
            if (require_even_alignment) {
                offset &= (uint16_t)~1U;
            }
            *crop_x = (uint16_t)(*crop_x + offset);
            *crop_width = fitted_width;
        } else if (source_ratio < target_ratio) {
            uint16_t fitted_height =
                (uint16_t)(((uint32_t)(*crop_width) * target_height) /
                           target_width);
            fitted_height &= (uint16_t)~1U;
            ESP_RETURN_ON_FALSE(fitted_height >= 16U,
                                ESP_ERR_INVALID_SIZE,
                                TAG,
                                "cover crop height is too small");
            uint16_t offset =
                (uint16_t)((*crop_height - fitted_height) / 2U);
            if (require_even_alignment) {
                offset &= (uint16_t)~1U;
            }
            *crop_y = (uint16_t)(*crop_y + offset);
            *crop_height = fitted_height;
        }
    }

    ESP_RETURN_ON_FALSE((!require_even_alignment ||
                         ((*crop_x | *crop_y | *crop_width | *crop_height) & 1U) == 0U) &&
                            *crop_width >= 16U && *crop_height >= 16U &&
                            (uint32_t)*crop_x + *crop_width <= source_width &&
                            (uint32_t)*crop_y + *crop_height <= source_height,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "invalid fitted source crop");
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.last_crop_x = *crop_x;
    handle->stats.last_crop_y = *crop_y;
    handle->stats.last_crop_width = *crop_width;
    handle->stats.last_crop_height = *crop_height;
    taskEXIT_CRITICAL(&handle->stats_lock);
    return ESP_OK;
}

static esp_err_t video_frame_convert_i420_scaled(video_frame_converter_handle_t handle,
                                                 const uint8_t *i420,
                                                 uint16_t source_width,
                                                 uint16_t source_height,
                                                 uint16_t crop_x,
                                                 uint16_t crop_y,
                                                 uint16_t crop_width,
                                                 uint16_t crop_height,
                                                 uint16_t *output)
{
    const size_t luma_size = (size_t)source_width * source_height;
    const uint8_t *plane_y = i420;
    const uint8_t *plane_u = plane_y + luma_size;
    const uint8_t *plane_v = plane_u + (luma_size / 4U);
    const uint32_t chroma_stride = source_width / 2U;
    uint16_t render_width = 0;
    uint16_t render_height = 0;
    uint16_t offset_x = 0;
    uint16_t offset_y = 0;

    video_frame_fit_inside(crop_width,
                           crop_height,
                           handle->config.output_width,
                           handle->config.output_height,
                           handle->config.prevent_upscale,
                           &render_width,
                           &render_height,
                           &offset_x,
                           &offset_y);
    ESP_RETURN_ON_FALSE(render_width > 0U && render_height > 0U,
                        ESP_ERR_INVALID_SIZE,
                        TAG,
                        "invalid software conversion layout");

    video_frame_record_render_layout(handle,
                                     render_width,
                                     render_height,
                                     offset_x,
                                     offset_y);
    memset(output,
           0,
           (size_t)handle->config.output_width * handle->config.output_height * sizeof(*output));
    const uint32_t source_x_step = ((uint32_t)crop_width << 16) / render_width;
    const uint32_t source_y_step = ((uint32_t)crop_height << 16) / render_height;
    uint32_t source_y_acc = 0U;

    for (uint32_t out_y = 0; out_y < render_height; ++out_y) {
        uint32_t source_y = crop_y + (source_y_acc >> 16);
        uint32_t source_x_acc = 0U;
        uint16_t *output_row =
            output + ((size_t)(out_y + offset_y) * handle->config.output_width) + offset_x;

        for (uint32_t out_x = 0; out_x < render_width; ++out_x) {
            uint32_t source_x = crop_x + (source_x_acc >> 16);
            int chroma_u = plane_u[((source_y / 2U) * chroma_stride) + (source_x / 2U)] - 128;
            int chroma_v = plane_v[((source_y / 2U) * chroma_stride) + (source_x / 2U)] - 128;
            output_row[out_x] = video_frame_yuv_to_rgb565(
                handle->luma_terms,
                plane_y[(source_y * source_width) + source_x],
                chroma_u,
                chroma_v,
                handle->config.output_rgb565_byte_swap);
            source_x_acc += source_x_step;
        }
        source_y_acc += source_y_step;
    }
    return ESP_OK;
}

esp_err_t video_frame_converter_create(const video_frame_converter_config_t *config,
                                       video_frame_converter_handle_t *out_handle)
{
    ESP_RETURN_ON_FALSE(config != NULL && out_handle != NULL &&
                            config->output_width > 0U && config->output_height > 0U &&
                            video_frame_fit_mode_is_valid(config->fit_mode) &&
                            ((config->output_width | config->output_height |
                              config->source_crop_x | config->source_crop_y |
                              config->source_crop_width | config->source_crop_height) & 1U) == 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid converter configuration");

    /* This table is read four times per 2x2 block. Keep the small converter
     * state in internal RAM; frame-sized buffers remain in PSRAM. */
    video_frame_converter_handle_t handle =
        heap_caps_calloc(1, sizeof(*handle), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, TAG, "converter allocation failed");
    portMUX_INITIALIZE(&handle->stats_lock);
    handle->config = *config;
    handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
    for (int index = 0; index < 256; ++index) {
        int luma = index - 16;
        handle->luma_terms[index] = 298 * (luma > 0 ? luma : 0);
    }
    size_t output_packed_size =
        (size_t)config->output_width * config->output_height * 3U / 2U;
    size_t source_packed_size =
        (size_t)config->source_crop_width * config->source_crop_height * 3U / 2U;
    size_t packed_data_size =
        source_packed_size > output_packed_size ?
            source_packed_size : output_packed_size;
    handle->packed_yuv420_size = video_frame_align_up(packed_data_size,
                                                       VIDEO_FRAME_CACHE_LINE_SIZE);
    handle->packed_yuv420 = app_memory_aligned_calloc_psram(
        VIDEO_FRAME_CACHE_LINE_SIZE,
        1,
        handle->packed_yuv420_size,
        MALLOC_CAP_DMA | MALLOC_CAP_CACHE_ALIGNED);
    const ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    if (handle->packed_yuv420 != NULL &&
        ppa_register_client(&ppa_config, &handle->ppa_client) == ESP_OK) {
        handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_PPA;
    } else {
        heap_caps_free(handle->packed_yuv420);
        handle->packed_yuv420 = NULL;
        handle->packed_yuv420_size = 0;
        handle->ppa_client = NULL;
    }
    *out_handle = handle;

    ESP_LOGI(TAG,
             "video converter ready: mode=%s fit=%s upscale=%s "
             "crop=%ux%u+%u+%u "
             "output=RGB565-%ux%u i420_staging=%u",
             video_frame_converter_mode_name(handle->last_mode),
             video_frame_fit_mode_name(config->fit_mode),
             config->prevent_upscale ? "deny" : "allow",
             config->source_crop_width,
             config->source_crop_height,
             config->source_crop_x,
             config->source_crop_y,
             config->output_width,
             config->output_height,
             (unsigned)handle->packed_yuv420_size);
    return ESP_OK;
}

void video_frame_converter_destroy(video_frame_converter_handle_t handle)
{
    if (handle == NULL) {
        return;
    }
    if (handle->ppa_client != NULL) {
        (void)ppa_unregister_client(handle->ppa_client);
    }
    heap_caps_free(handle->packed_yuv420);
    heap_caps_free(handle);
}

esp_err_t video_frame_converter_i420_to_rgb565(video_frame_converter_handle_t handle,
                                               const uint8_t *i420,
                                               uint16_t source_width,
                                               uint16_t source_height,
                                               uint16_t *output,
                                               video_frame_converter_mode_t *mode_used)
{
    uint16_t crop_x = 0;
    uint16_t crop_y = 0;
    uint16_t crop_width = 0;
    uint16_t crop_height = 0;

    ESP_RETURN_ON_FALSE(handle != NULL && i420 != NULL && output != NULL &&
                            source_width >= 16U && source_height >= 16U &&
                            (source_width & 1U) == 0U && (source_height & 1U) == 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid I420 conversion input");
    ESP_RETURN_ON_ERROR(video_frame_resolve_crop(handle,
                                                 source_width,
                                                 source_height,
                                                 &crop_x,
                                                 &crop_y,
                                                 &crop_width,
                                                 &crop_height),
                        TAG,
                        "resolve I420 crop failed");
    ESP_RETURN_ON_ERROR(video_frame_apply_fit_crop(handle,
                                                   VIDEO_FRAME_ROTATION_CLOCKWISE_0,
                                                   true,
                                                   source_width,
                                                   source_height,
                                                   &crop_x,
                                                   &crop_y,
                                                   &crop_width,
                                                   &crop_height),
                        TAG,
                        "resolve I420 fit failed");
    if (handle->ppa_client != NULL) {
        esp_err_t ppa_ret = video_frame_convert_yuv420_ppa(handle,
                                                           i420,
                                                           source_width,
                                                           source_height,
                                                           crop_x,
                                                           crop_y,
                                                           crop_width,
                                                           crop_height,
                                                           false,
                                                           output);
        if (ppa_ret == ESP_OK) {
            handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_PPA;
            if (mode_used != NULL) {
                *mode_used = VIDEO_FRAME_CONVERTER_MODE_PPA;
            }
            return ESP_OK;
        }
        uint32_t ppa_failures = 0;
        taskENTER_CRITICAL(&handle->stats_lock);
        handle->stats.ppa_failures++;
        ppa_failures = handle->stats.ppa_failures;
        taskEXIT_CRITICAL(&handle->stats_lock);
        if (ppa_failures == 1U || (ppa_failures % 100U) == 0U) {
            ESP_LOGW(TAG,
                     "PPA conversion fallback: ret=%s failures=%lu",
                     esp_err_to_name(ppa_ret),
                     (unsigned long)ppa_failures);
        }
    }

    handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
    if (mode_used != NULL) {
        *mode_used = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
    }
    int64_t software_started_us = esp_timer_get_time();
    esp_err_t software_ret = ESP_OK;
    if (crop_width == handle->config.output_width &&
        crop_height == handle->config.output_height) {
        software_ret = video_frame_convert_i420_region(handle,
                                                        i420,
                                                        source_width,
                                                        source_height,
                                                        crop_x,
                                                        crop_y,
                                                        crop_width,
                                                        crop_height,
                                                        output);
    } else {
        software_ret = video_frame_convert_i420_scaled(handle,
                                                        i420,
                                                        source_width,
                                                        source_height,
                                                        crop_x,
                                                        crop_y,
                                                        crop_width,
                                                        crop_height,
                                                        output);
    }
    uint32_t software_us = (uint32_t)(esp_timer_get_time() - software_started_us);
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.software_frames++;
    handle->stats.software_time_us += software_us;
    if (software_us > handle->stats.software_max_us) {
        handle->stats.software_max_us = software_us;
    }
    taskEXIT_CRITICAL(&handle->stats_lock);
    return software_ret;
}

esp_err_t video_frame_converter_ouyy_evyy_to_rgb565(
    video_frame_converter_handle_t handle,
    const uint8_t *ouyy_evyy,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t *output,
    video_frame_converter_mode_t *mode_used)
{
    uint16_t crop_x = 0U;
    uint16_t crop_y = 0U;
    uint16_t crop_width = 0U;
    uint16_t crop_height = 0U;

    ESP_RETURN_ON_FALSE(handle != NULL && ouyy_evyy != NULL && output != NULL &&
                            source_width >= 16U && source_height >= 16U &&
                            (source_width & 1U) == 0U &&
                            (source_height & 1U) == 0U,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid O_UYY_E_VYY conversion input");
    ESP_RETURN_ON_ERROR(video_frame_resolve_crop(handle,
                                                 source_width,
                                                 source_height,
                                                 &crop_x,
                                                 &crop_y,
                                                 &crop_width,
                                                 &crop_height),
                        TAG,
                        "resolve O_UYY_E_VYY crop failed");
    ESP_RETURN_ON_ERROR(video_frame_apply_fit_crop(handle,
                                                   VIDEO_FRAME_ROTATION_CLOCKWISE_0,
                                                   true,
                                                   source_width,
                                                   source_height,
                                                   &crop_x,
                                                   &crop_y,
                                                   &crop_width,
                                                   &crop_height),
                        TAG,
                        "resolve O_UYY_E_VYY fit failed");
    ESP_RETURN_ON_FALSE(handle->ppa_client != NULL,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "O_UYY_E_VYY conversion requires PPA");

    esp_err_t ret = video_frame_convert_yuv420_ppa(handle,
                                                   ouyy_evyy,
                                                   source_width,
                                                   source_height,
                                                   crop_x,
                                                   crop_y,
                                                   crop_width,
                                                   crop_height,
                                                   true,
                                                   output);
    if (ret == ESP_OK) {
        handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_PPA;
        if (mode_used != NULL) {
            *mode_used = VIDEO_FRAME_CONVERTER_MODE_PPA;
        }
        return ESP_OK;
    }

    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.ppa_failures++;
    taskEXIT_CRITICAL(&handle->stats_lock);
    return ret;
}

static esp_err_t video_frame_convert_rgb565_ppa(video_frame_converter_handle_t handle,
                                                const uint16_t *rgb565,
                                                uint16_t source_width,
                                                uint16_t source_height,
                                                uint16_t source_stride_pixels,
                                                uint16_t crop_x,
                                                uint16_t crop_y,
                                                uint16_t crop_width,
                                                uint16_t crop_height,
                                                bool input_byte_swap,
                                                video_frame_rotation_t rotation,
                                                uint16_t *output)
{
    const uint16_t output_width = handle->config.output_width;
    const uint16_t output_height = handle->config.output_height;
    const size_t output_size =
        (size_t)output_width * output_height * sizeof(*output);
    video_frame_ppa_fit_t fit = {0};

    ESP_RETURN_ON_FALSE(handle->ppa_client != NULL &&
                            source_stride_pixels >= source_width &&
                            video_frame_resolve_ppa_fit(crop_width,
                                                        crop_height,
                                                        output_width,
                                                        output_height,
                                                        rotation,
                                                        handle->config.prevent_upscale,
                                                        &fit) &&
                            ((uintptr_t)output &
                             (VIDEO_FRAME_CACHE_LINE_SIZE - 1U)) == 0U &&
                            (output_size &
                             (VIDEO_FRAME_CACHE_LINE_SIZE - 1U)) == 0U,
                        ESP_ERR_NOT_SUPPORTED,
                        TAG,
                        "PPA RGB565 conversion buffer is not compatible");

    video_frame_record_render_layout(handle,
                                     fit.render_width,
                                     fit.render_height,
                                     fit.offset_x,
                                     fit.offset_y);
    int64_t started_us = esp_timer_get_time();
    if (fit.render_width != output_width || fit.render_height != output_height) {
        memset(output, 0, output_size);
        ESP_RETURN_ON_ERROR(esp_cache_msync(output,
                                            output_size,
                                            ESP_CACHE_MSYNC_FLAG_DIR_C2M),
                            TAG,
                            "sync PPA RGB565 letterbox background failed");
    }
    int64_t prepared_us = esp_timer_get_time();
    const ppa_srm_oper_config_t operation = {
        .in = {
            .buffer = (void *)rgb565,
            .pic_w = source_stride_pixels,
            .pic_h = source_height,
            .block_w = crop_width,
            .block_h = crop_height,
            .block_offset_x = crop_x,
            .block_offset_y = crop_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        .out = {
            .buffer = output,
            .buffer_size = output_size,
            .pic_w = output_width,
            .pic_h = output_height,
            .block_offset_x = fit.offset_x,
            .block_offset_y = fit.offset_y,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            .yuv_range = PPA_COLOR_RANGE_LIMIT,
            .yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
        },
        /* The PPA API defines positive angles counterclockwise. */
        .rotation_angle = video_frame_rotation_to_ppa(rotation),
        .scale_x = fit.scale,
        .scale_y = fit.scale,
        .byte_swap = input_byte_swap,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    ESP_RETURN_ON_ERROR(ppa_do_scale_rotate_mirror(handle->ppa_client, &operation),
                        TAG,
                        "PPA RGB565 conversion failed");
    int64_t ppa_done_us = esp_timer_get_time();
    if (handle->config.output_rgb565_byte_swap) {
        video_frame_swap_rgb565_bytes(output, (size_t)output_width * output_height);
    }
    int64_t done_us = esp_timer_get_time();

    uint32_t prepare_us = (uint32_t)(prepared_us - started_us);
    uint32_t ppa_us = (uint32_t)(ppa_done_us - prepared_us);
    uint32_t swap_us = (uint32_t)(done_us - ppa_done_us);
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.ppa_frames++;
    handle->stats.pack_time_us += prepare_us;
    handle->stats.ppa_time_us += ppa_us;
    handle->stats.swap_time_us += swap_us;
    if (prepare_us > handle->stats.pack_max_us) {
        handle->stats.pack_max_us = prepare_us;
    }
    if (ppa_us > handle->stats.ppa_max_us) {
        handle->stats.ppa_max_us = ppa_us;
    }
    if (swap_us > handle->stats.swap_max_us) {
        handle->stats.swap_max_us = swap_us;
    }
    taskEXIT_CRITICAL(&handle->stats_lock);
    return ESP_OK;
}

static esp_err_t video_frame_convert_rgb565_scaled(
    video_frame_converter_handle_t handle,
    const uint16_t *rgb565,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t source_stride_pixels,
    uint16_t crop_x,
    uint16_t crop_y,
    uint16_t crop_width,
    uint16_t crop_height,
    bool input_byte_swap,
    video_frame_rotation_t rotation,
    uint16_t *output)
{
    uint16_t render_width = 0U;
    uint16_t render_height = 0U;
    uint16_t offset_x = 0U;
    uint16_t offset_y = 0U;

    const bool swap_axes = video_frame_rotation_swaps_axes(rotation);
    const uint16_t rotated_width = swap_axes ? crop_height : crop_width;
    const uint16_t rotated_height = swap_axes ? crop_width : crop_height;
    video_frame_fit_inside(rotated_width,
                           rotated_height,
                           handle->config.output_width,
                           handle->config.output_height,
                           handle->config.prevent_upscale,
                           &render_width,
                           &render_height,
                           &offset_x,
                           &offset_y);
    ESP_RETURN_ON_FALSE(render_width > 0U && render_height > 0U &&
                             source_stride_pixels >= source_width &&
                             (uint32_t)crop_x + crop_width <= source_width &&
                             (uint32_t)crop_y + crop_height <= source_height &&
                             video_frame_rotation_is_valid(rotation),
                         ESP_ERR_INVALID_SIZE,
                         TAG,
                         "invalid software RGB565 conversion layout");

    video_frame_record_render_layout(handle,
                                     render_width,
                                     render_height,
                                     offset_x,
                                     offset_y);
    memset(output,
           0,
           (size_t)handle->config.output_width *
               handle->config.output_height * sizeof(*output));
    const uint32_t rotated_x_step =
        ((uint32_t)rotated_width << 16) / render_width;
    const uint32_t rotated_y_step =
        ((uint32_t)rotated_height << 16) / render_height;
    const bool swap_value =
        input_byte_swap != handle->config.output_rgb565_byte_swap;
    uint32_t rotated_y_acc = 0U;

    for (uint32_t out_y = 0; out_y < render_height; ++out_y) {
        uint32_t rotated_y = rotated_y_acc >> 16;
        uint32_t rotated_x_acc = 0U;
        uint16_t *output_row =
            output + ((size_t)(out_y + offset_y) *
                      handle->config.output_width) + offset_x;

        for (uint32_t out_x = 0; out_x < render_width; ++out_x) {
            uint32_t rotated_x = rotated_x_acc >> 16;
            uint32_t source_x = 0U;
            uint32_t source_y = 0U;
            switch (rotation) {
            case VIDEO_FRAME_ROTATION_CLOCKWISE_90:
                source_x = rotated_y;
                source_y = (uint32_t)crop_height - 1U - rotated_x;
                break;
            case VIDEO_FRAME_ROTATION_CLOCKWISE_180:
                source_x = (uint32_t)crop_width - 1U - rotated_x;
                source_y = (uint32_t)crop_height - 1U - rotated_y;
                break;
            case VIDEO_FRAME_ROTATION_CLOCKWISE_270:
                source_x = (uint32_t)crop_width - 1U - rotated_y;
                source_y = rotated_x;
                break;
            case VIDEO_FRAME_ROTATION_CLOCKWISE_0:
            default:
                source_x = rotated_x;
                source_y = rotated_y;
                break;
            }
            uint16_t value =
                rgb565[((size_t)(crop_y + source_y) * source_stride_pixels) +
                       crop_x + source_x];
            output_row[out_x] = swap_value ?
                                    (uint16_t)((value >> 8) | (value << 8)) :
                                    value;
            rotated_x_acc += rotated_x_step;
        }
        rotated_y_acc += rotated_y_step;
    }
    return ESP_OK;
}

esp_err_t video_frame_converter_rgb565_to_rgb565(
    video_frame_converter_handle_t handle,
    const uint16_t *rgb565,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t source_stride_pixels,
    bool input_rgb565_byte_swap,
    video_frame_rotation_t rotation,
    uint16_t *output,
    video_frame_converter_mode_t *mode_used)
{
    uint16_t crop_x = 0U;
    uint16_t crop_y = 0U;
    uint16_t crop_width = 0U;
    uint16_t crop_height = 0U;

    ESP_RETURN_ON_FALSE(handle != NULL && rgb565 != NULL && output != NULL &&
                             source_width > 0U && source_height > 0U &&
                             source_stride_pixels >= source_width &&
                             video_frame_rotation_is_valid(rotation),
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid RGB565 conversion input");
    ESP_RETURN_ON_ERROR(video_frame_resolve_crop(handle,
                                                 source_width,
                                                 source_height,
                                                 &crop_x,
                                                 &crop_y,
                                                 &crop_width,
                                                 &crop_height),
                        TAG,
                        "resolve RGB565 crop failed");
    ESP_RETURN_ON_ERROR(video_frame_apply_fit_crop(handle,
                                                   rotation,
                                                   false,
                                                   source_width,
                                                   source_height,
                                                   &crop_x,
                                                   &crop_y,
                                                   &crop_width,
                                                   &crop_height),
                        TAG,
                        "resolve RGB565 fit failed");
    if (handle->ppa_client != NULL) {
        esp_err_t ppa_ret = video_frame_convert_rgb565_ppa(handle,
                                                           rgb565,
                                                           source_width,
                                                           source_height,
                                                           source_stride_pixels,
                                                           crop_x,
                                                           crop_y,
                                                           crop_width,
                                                           crop_height,
                                                           input_rgb565_byte_swap,
                                                           rotation,
                                                           output);
        if (ppa_ret == ESP_OK) {
            handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_PPA;
            if (mode_used != NULL) {
                *mode_used = VIDEO_FRAME_CONVERTER_MODE_PPA;
            }
            return ESP_OK;
        }

        uint32_t ppa_failures = 0U;
        taskENTER_CRITICAL(&handle->stats_lock);
        handle->stats.ppa_failures++;
        ppa_failures = handle->stats.ppa_failures;
        taskEXIT_CRITICAL(&handle->stats_lock);
        if (ppa_failures == 1U || (ppa_failures % 100U) == 0U) {
            ESP_LOGW(TAG,
                     "PPA RGB565 conversion fallback: ret=%s failures=%lu",
                     esp_err_to_name(ppa_ret),
                     (unsigned long)ppa_failures);
        }
    }

    handle->last_mode = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
    if (mode_used != NULL) {
        *mode_used = VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
    }
    int64_t software_started_us = esp_timer_get_time();
    esp_err_t software_ret = video_frame_convert_rgb565_scaled(
        handle,
        rgb565,
        source_width,
        source_height,
        source_stride_pixels,
        crop_x,
        crop_y,
        crop_width,
        crop_height,
        input_rgb565_byte_swap,
        rotation,
        output);
    uint32_t software_us =
        (uint32_t)(esp_timer_get_time() - software_started_us);
    taskENTER_CRITICAL(&handle->stats_lock);
    handle->stats.software_frames++;
    handle->stats.software_time_us += software_us;
    if (software_us > handle->stats.software_max_us) {
        handle->stats.software_max_us = software_us;
    }
    taskEXIT_CRITICAL(&handle->stats_lock);
    return software_ret;
}

video_frame_converter_mode_t video_frame_converter_get_mode(video_frame_converter_handle_t handle)
{
    return handle != NULL ? handle->last_mode : VIDEO_FRAME_CONVERTER_MODE_SOFTWARE;
}

void video_frame_converter_get_stats(video_frame_converter_handle_t handle,
                                     video_frame_converter_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    if (handle == NULL) {
        return;
    }
    taskENTER_CRITICAL(&handle->stats_lock);
    *stats = handle->stats;
    taskEXIT_CRITICAL(&handle->stats_lock);
}

const char *video_frame_converter_mode_name(video_frame_converter_mode_t mode)
{
    switch (mode) {
    case VIDEO_FRAME_CONVERTER_MODE_PPA:
        return "ppa";
    case VIDEO_FRAME_CONVERTER_MODE_SOFTWARE:
    default:
        return "software";
    }
}

const char *video_frame_rotation_name(video_frame_rotation_t rotation)
{
    switch (rotation) {
    case VIDEO_FRAME_ROTATION_CLOCKWISE_90:
        return "cw90";
    case VIDEO_FRAME_ROTATION_CLOCKWISE_180:
        return "cw180";
    case VIDEO_FRAME_ROTATION_CLOCKWISE_270:
        return "cw270";
    case VIDEO_FRAME_ROTATION_CLOCKWISE_0:
    default:
        return "cw0";
    }
}

const char *video_frame_fit_mode_name(video_frame_fit_mode_t mode)
{
    switch (mode) {
    case VIDEO_FRAME_FIT_COVER:
        return "cover";
    case VIDEO_FRAME_FIT_CONTAIN:
    default:
        return "contain";
    }
}
