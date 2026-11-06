#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct video_frame_converter *video_frame_converter_handle_t;

typedef enum {
    VIDEO_FRAME_CONVERTER_MODE_SOFTWARE = 0,
    VIDEO_FRAME_CONVERTER_MODE_PPA,
} video_frame_converter_mode_t;

typedef enum {
    VIDEO_FRAME_ROTATION_CLOCKWISE_0 = 0,
    VIDEO_FRAME_ROTATION_CLOCKWISE_90 = 90,
    VIDEO_FRAME_ROTATION_CLOCKWISE_180 = 180,
    VIDEO_FRAME_ROTATION_CLOCKWISE_270 = 270,
} video_frame_rotation_t;

typedef enum {
    VIDEO_FRAME_FIT_CONTAIN = 0,
    VIDEO_FRAME_FIT_COVER,
} video_frame_fit_mode_t;

typedef struct {
    uint16_t output_width;
    uint16_t output_height;
    /* Optional even-aligned source crop. A zero width/height selects the
     * complete source frame. The P4 PPA uses its 1/16 scale steps for
     * aspect-preserving rendering, with software fallback when unsupported. */
    uint16_t source_crop_x;
    uint16_t source_crop_y;
    uint16_t source_crop_width;
    uint16_t source_crop_height;
    video_frame_fit_mode_t fit_mode;
    /* Keep sources smaller than the viewport at native size and center them
     * instead of manufacturing detail through upscaling. */
    bool prevent_upscale;
    bool output_rgb565_byte_swap;
} video_frame_converter_config_t;

typedef struct {
    uint32_t ppa_frames;
    uint32_t software_frames;
    uint32_t ppa_failures;
    uint64_t pack_time_us;
    uint32_t pack_max_us;
    uint64_t ppa_time_us;
    uint32_t ppa_max_us;
    uint64_t swap_time_us;
    uint32_t swap_max_us;
    uint64_t software_time_us;
    uint32_t software_max_us;
    uint16_t last_crop_x;
    uint16_t last_crop_y;
    uint16_t last_crop_width;
    uint16_t last_crop_height;
    uint16_t last_render_width;
    uint16_t last_render_height;
    uint16_t last_offset_x;
    uint16_t last_offset_y;
} video_frame_converter_stats_t;

esp_err_t video_frame_converter_create(const video_frame_converter_config_t *config,
                                       video_frame_converter_handle_t *out_handle);
void video_frame_converter_destroy(video_frame_converter_handle_t handle);

esp_err_t video_frame_converter_i420_to_rgb565(video_frame_converter_handle_t handle,
                                               const uint8_t *i420,
                                               uint16_t source_width,
                                               uint16_t source_height,
                                               uint16_t *output,
                                               video_frame_converter_mode_t *mode_used);

/* ESP32-P4 camera and H264 hardware use packed O_UYY_E_VYY YUV420. Keep this
 * contract separate from planar I420 so callers cannot silently reinterpret
 * the same bytes with the wrong layout. */
esp_err_t video_frame_converter_ouyy_evyy_to_rgb565(
    video_frame_converter_handle_t handle,
    const uint8_t *ouyy_evyy,
    uint16_t source_width,
    uint16_t source_height,
    uint16_t *output,
    video_frame_converter_mode_t *mode_used);

/* Rotates and scales a packed RGB565 source into the configured viewport while
 * preserving aspect ratio. source_stride_pixels may be wider than source_width
 * when a hardware decoder pads each row. */
esp_err_t video_frame_converter_rgb565_to_rgb565(video_frame_converter_handle_t handle,
                                                 const uint16_t *rgb565,
                                                 uint16_t source_width,
                                                 uint16_t source_height,
                                                 uint16_t source_stride_pixels,
                                                 bool input_rgb565_byte_swap,
                                                 video_frame_rotation_t rotation,
                                                 uint16_t *output,
                                                 video_frame_converter_mode_t *mode_used);

video_frame_converter_mode_t video_frame_converter_get_mode(video_frame_converter_handle_t handle);
void video_frame_converter_get_stats(video_frame_converter_handle_t handle,
                                     video_frame_converter_stats_t *stats);
const char *video_frame_converter_mode_name(video_frame_converter_mode_t mode);
const char *video_frame_rotation_name(video_frame_rotation_t rotation);
const char *video_frame_fit_mode_name(video_frame_fit_mode_t mode);
