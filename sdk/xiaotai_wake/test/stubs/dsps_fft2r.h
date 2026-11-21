// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
extern int dsps_fft_w_table_size;
extern uint8_t dsps_fft2r_initialized;

esp_err_t dsps_fft2r_init_fc32(float *table, int size);
void dsps_fft2r_deinit_fc32(void);
esp_err_t dsps_fft2r_fc32(float *pairs, int n);
esp_err_t dsps_fft2r_fc32_ansi(float *pairs, int n);
esp_err_t dsps_bit_rev2r_fc32(float *pairs, int n);
