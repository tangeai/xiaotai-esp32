// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef uint32_t TickType_t;
#define pdMS_TO_TICKS(ms) (ms)
#define configMAX_PRIORITIES 25

typedef struct esp_afe_sr_data esp_afe_sr_data_t;
typedef struct afe_config afe_config_t;

typedef struct {
    int ret_value, data_size;
    int16_t *data;
} afe_fetch_result_t;

typedef struct {
    esp_afe_sr_data_t *(*create_from_config)(afe_config_t *config);
    int (*feed)(esp_afe_sr_data_t *data, const int16_t *pcm);
    afe_fetch_result_t *(*fetch_with_delay)(esp_afe_sr_data_t *data, TickType_t ticks);
    void (*destroy)(esp_afe_sr_data_t *data);
    int (*get_feed_chunksize)(esp_afe_sr_data_t *data);
    int (*get_fetch_chunksize)(esp_afe_sr_data_t *data);
    int (*get_feed_channel_num)(esp_afe_sr_data_t *data);
    int (*get_fetch_channel_num)(esp_afe_sr_data_t *data);
    int (*get_samp_rate)(esp_afe_sr_data_t *data);
} esp_afe_sr_iface_t;
