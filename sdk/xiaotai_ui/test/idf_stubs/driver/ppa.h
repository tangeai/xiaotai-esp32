#pragma once
/* The part of the ESP-IDF PPA driver's interface the UI uses; the test supplies the functions. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct ppa_client_t *ppa_client_handle_t;
typedef enum { PPA_OPERATION_SRM, PPA_OPERATION_BLEND, PPA_OPERATION_FILL } ppa_operation_t;
typedef enum { PPA_DATA_BURST_LENGTH_128 = 128 } ppa_data_burst_length_t;
typedef enum { PPA_TRANS_MODE_BLOCKING, PPA_TRANS_MODE_NON_BLOCKING } ppa_trans_mode_t;
typedef enum { PPA_SRM_ROTATION_ANGLE_0 } ppa_srm_rotation_angle_t;
typedef enum { PPA_SRM_COLOR_MODE_RGB565 = 0x35363552 } ppa_srm_color_mode_t;
typedef enum { PPA_FILL_COLOR_MODE_RGB565 = 0x35363552 } ppa_fill_color_mode_t;
typedef struct { uint8_t b, g, r, a; } color_pixel_argb8888_data_t;

typedef struct {
    ppa_operation_t oper_type;
    uint32_t max_pending_trans_num;
    ppa_data_burst_length_t data_burst_length;
} ppa_client_config_t;

typedef struct {
    const void *buffer;
    uint32_t pic_w, pic_h, block_w, block_h, block_offset_x, block_offset_y;
    ppa_srm_color_mode_t srm_cm;
} ppa_in_pic_blk_config_t;

typedef struct {
    void *buffer;
    uint32_t buffer_size, pic_w, pic_h, block_offset_x, block_offset_y;
    union {
        ppa_srm_color_mode_t srm_cm;
        ppa_fill_color_mode_t fill_cm;
    };
} ppa_out_pic_blk_config_t;

typedef struct {
    ppa_in_pic_blk_config_t in;
    ppa_out_pic_blk_config_t out;
    ppa_srm_rotation_angle_t rotation_angle;
    float scale_x, scale_y;
    ppa_trans_mode_t mode;
} ppa_srm_oper_config_t;

typedef struct {
    ppa_out_pic_blk_config_t out;
    uint32_t fill_block_w, fill_block_h;
    color_pixel_argb8888_data_t fill_argb_color;
    ppa_trans_mode_t mode;
} ppa_fill_oper_config_t;

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client);
esp_err_t ppa_unregister_client(ppa_client_handle_t ppa_client);
esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t ppa_client, const ppa_srm_oper_config_t *config);
esp_err_t ppa_do_fill(ppa_client_handle_t ppa_client, const ppa_fill_oper_config_t *config);
