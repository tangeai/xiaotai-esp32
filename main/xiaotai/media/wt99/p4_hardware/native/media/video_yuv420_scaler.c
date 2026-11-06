#include "video_yuv420_scaler.h"

#include <stdlib.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"

static const char *TAG = "video_scaler";

#define VIDEO_YUV420_SCALE_DENOMINATOR 16U

#if CONFIG_CACHE_L2_CACHE_LINE_SIZE > CONFIG_CACHE_L1_CACHE_LINE_SIZE
#define VIDEO_YUV420_CACHE_LINE_SIZE CONFIG_CACHE_L2_CACHE_LINE_SIZE
#else
#define VIDEO_YUV420_CACHE_LINE_SIZE CONFIG_CACHE_L1_CACHE_LINE_SIZE
#endif

struct video_yuv420_scaler {
    ppa_client_handle_t ppa_client;
    uint8_t *output_buffer;
    size_t output_buffer_size;
    size_t output_data_len;
    video_yuv420_scaler_config_t config;
    uint16_t crop_width;
    uint16_t crop_height;
    uint16_t crop_x;
    uint16_t crop_y;
    uint8_t scale_step;
};

static size_t video_yuv420_data_size(uint16_t width, uint16_t height)
{
    return ((size_t)width * (size_t)height * 3U) / 2U;
}

static size_t video_yuv420_align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1U) & ~(alignment - 1U);
}

static bool video_yuv420_config_valid(const video_yuv420_scaler_config_t *config)
{
    return config != NULL &&
           config->input_width > 0U && config->input_height > 0U &&
           config->output_width > 0U && config->output_height > 0U &&
           (config->input_width & 1U) == 0U && (config->input_height & 1U) == 0U &&
           (config->output_width & 1U) == 0U && (config->output_height & 1U) == 0U &&
           (config->rotate_ccw90 ? config->output_height : config->output_width) <= config->input_width &&
           (config->rotate_ccw90 ? config->output_width : config->output_height) <= config->input_height;
}

static bool video_yuv420_select_geometry(const video_yuv420_scaler_config_t *config,
                                         uint16_t *crop_width,
                                         uint16_t *crop_height,
                                         uint16_t *crop_x,
                                         uint16_t *crop_y,
                                         uint8_t *scale_step)
{
    /* Select the crop in SENSOR axes, before PPA exchanges the output axes. */
    const uint32_t scaled_width_numerator = (uint32_t)(config->rotate_ccw90 ? config->output_height : config->output_width) * VIDEO_YUV420_SCALE_DENOMINATOR;
    const uint32_t scaled_height_numerator = (uint32_t)(config->rotate_ccw90 ? config->output_width : config->output_height) * VIDEO_YUV420_SCALE_DENOMINATOR;

    /* PPA YUV420 masks the fractional scale bit 0, so only even 1/16 steps are exact. */
    for (uint8_t step = 2U; step <= VIDEO_YUV420_SCALE_DENOMINATOR; step += 2U) {
        if ((scaled_width_numerator % step) != 0U || (scaled_height_numerator % step) != 0U) {
            continue;
        }

        uint32_t candidate_width = scaled_width_numerator / step;
        uint32_t candidate_height = scaled_height_numerator / step;
        if (candidate_width > config->input_width || candidate_height > config->input_height ||
            (candidate_width & 1U) != 0U || (candidate_height & 1U) != 0U) {
            continue;
        }

        uint32_t x = ((uint32_t)config->input_width - candidate_width) / 2U;
        uint32_t y = ((uint32_t)config->input_height - candidate_height) / 2U;
        x &= ~1U;
        y &= ~1U;

        *crop_width = (uint16_t)candidate_width;
        *crop_height = (uint16_t)candidate_height;
        *crop_x = (uint16_t)x;
        *crop_y = (uint16_t)y;
        *scale_step = step;
        return true;
    }

    return false;
}

esp_err_t video_yuv420_scaler_create(const video_yuv420_scaler_config_t *config,
                                     video_yuv420_scaler_handle_t *out_handle)
{
    if (!video_yuv420_config_valid(config) || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_handle = NULL;

    struct video_yuv420_scaler *scaler =
        heap_caps_calloc(1, sizeof(*scaler), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (scaler == NULL) {
        return ESP_ERR_NO_MEM;
    }
    scaler->config = *config;

    if (!video_yuv420_select_geometry(config,
                                      &scaler->crop_width,
                                      &scaler->crop_height,
                                      &scaler->crop_x,
                                      &scaler->crop_y,
                                      &scaler->scale_step)) {
        free(scaler);
        return ESP_ERR_NOT_SUPPORTED;
    }

    scaler->output_data_len = video_yuv420_data_size(config->output_width, config->output_height);
    scaler->output_buffer_size =
        video_yuv420_align_up(scaler->output_data_len, VIDEO_YUV420_CACHE_LINE_SIZE);
    scaler->output_buffer =
        app_memory_aligned_alloc_psram(VIDEO_YUV420_CACHE_LINE_SIZE,
                                       scaler->output_buffer_size,
                                       MALLOC_CAP_DMA);
    if (scaler->output_buffer == NULL) {
        ESP_LOGE(TAG,
                 "YUV420 scaler output allocation failed: size=%u psram_largest=%u",
                 (unsigned)scaler->output_buffer_size,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        free(scaler);
        return ESP_ERR_NO_MEM;
    }

    ppa_client_config_t ppa_config = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t ret = ppa_register_client(&ppa_config, &scaler->ppa_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PPA scaler client registration failed: %s", esp_err_to_name(ret));
        free(scaler->output_buffer);
        free(scaler);
        return ret;
    }

    ESP_LOGI(TAG,
             "YUV420 scaler ready: input=%ux%u crop=%ux%u+%u+%u scale=%u/16 output=%ux%u buffer=%u",
             config->input_width,
             config->input_height,
             scaler->crop_width,
             scaler->crop_height,
             scaler->crop_x,
             scaler->crop_y,
             scaler->scale_step,
             config->output_width,
             config->output_height,
             (unsigned)scaler->output_buffer_size);

    *out_handle = scaler;
    return ESP_OK;
}

bool video_yuv420_scaler_matches(video_yuv420_scaler_handle_t handle,
                                 const video_yuv420_scaler_config_t *config)
{
    if (handle == NULL || config == NULL) {
        return false;
    }

    return handle->config.input_width == config->input_width &&
           handle->config.input_height == config->input_height &&
           handle->config.output_width == config->output_width &&
           handle->config.output_height == config->output_height &&
           handle->config.rotate_ccw90 == config->rotate_ccw90;
}

esp_err_t video_yuv420_scaler_warmup(video_yuv420_scaler_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t input_data_len =
        video_yuv420_data_size(handle->config.input_width, handle->config.input_height);
    size_t input_buffer_size =
        video_yuv420_align_up(input_data_len, VIDEO_YUV420_CACHE_LINE_SIZE);
    uint8_t *input = app_memory_aligned_alloc_psram(VIDEO_YUV420_CACHE_LINE_SIZE,
                                                    input_buffer_size,
                                                    MALLOC_CAP_DMA);
    if (input == NULL) {
        ESP_LOGE(TAG,
                 "YUV420 scaler warmup input allocation failed: size=%u psram_largest=%u",
                 (unsigned)input_buffer_size,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM |
                                                             MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }

    size_t luma_len = (size_t)handle->config.input_width * handle->config.input_height;
    memset(input, 16, luma_len);
    memset(input + luma_len, 128, input_data_len - luma_len);

    const uint8_t *output = NULL;
    size_t output_len = 0;
    int64_t started_us = esp_timer_get_time();
    esp_err_t ret = video_yuv420_scaler_process(handle,
                                                input,
                                                input_data_len,
                                                &output,
                                                &output_len);
    int64_t elapsed_us = esp_timer_get_time() - started_us;
    free(input);

    ESP_LOGI(TAG,
             "YUV420 scaler warmup: input=%ux%u output=%ux%u elapsed=%lldus ret=%s",
             handle->config.input_width,
             handle->config.input_height,
             handle->config.output_width,
             handle->config.output_height,
             (long long)elapsed_us,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t video_yuv420_scaler_process(video_yuv420_scaler_handle_t handle,
                                      const uint8_t *input,
                                      size_t input_len,
                                      const uint8_t **output,
                                      size_t *output_len)
{
    if (handle == NULL || input == NULL || output == NULL || output_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t expected_input_len =
        video_yuv420_data_size(handle->config.input_width, handle->config.input_height);
    if (input_len < expected_input_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    const float scale = (float)handle->scale_step / (float)VIDEO_YUV420_SCALE_DENOMINATOR;
    ppa_srm_oper_config_t operation = {
        .in.buffer = input,
        .in.pic_w = handle->config.input_width,
        .in.pic_h = handle->config.input_height,
        .in.block_w = handle->crop_width,
        .in.block_h = handle->crop_height,
        .in.block_offset_x = handle->crop_x,
        .in.block_offset_y = handle->crop_y,
        .in.srm_cm = PPA_SRM_COLOR_MODE_YUV420,

        .out.buffer = handle->output_buffer,
        .out.buffer_size = handle->output_buffer_size,
        .out.pic_w = handle->config.output_width,
        .out.pic_h = handle->config.output_height,
        .out.block_offset_x = 0,
        .out.block_offset_y = 0,
        .out.srm_cm = PPA_SRM_COLOR_MODE_YUV420,

        /* PPA positive angles are counterclockwise, not clockwise. */
        .rotation_angle = handle->config.rotate_ccw90 ? PPA_SRM_ROTATION_ANGLE_90 : PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale,
        .scale_y = scale,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(handle->ppa_client, &operation);
    if (ret != ESP_OK) {
        return ret;
    }

    /* PPA owns cache synchronization for this transaction; CPU never writes the output. */
    *output = handle->output_buffer;
    *output_len = handle->output_data_len;
    return ESP_OK;
}

void video_yuv420_scaler_destroy(video_yuv420_scaler_handle_t handle)
{
    if (handle == NULL) {
        return;
    }

    if (handle->ppa_client != NULL) {
        esp_err_t ret = ppa_unregister_client(handle->ppa_client);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PPA scaler client release failed: %s", esp_err_to_name(ret));
        }
    }
    free(handle->output_buffer);
    free(handle);
}
