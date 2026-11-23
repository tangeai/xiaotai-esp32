// SPDX-License-Identifier: MIT
#pragma once

enum { ESP_AGC_SUCCESS = 0 };
typedef enum { AGC_MODE_2 = 2, AGC_MODE_3 = 3 } agc_mode_t;

void *esp_agc_open(agc_mode_t mode, int rate);
void set_agc_config(void *handle, int gain_db, int limiter, int target_level_dbfs);
int esp_agc_process(void *handle, short *in, short *out, int samples, int rate);
void esp_agc_close(void *handle);
