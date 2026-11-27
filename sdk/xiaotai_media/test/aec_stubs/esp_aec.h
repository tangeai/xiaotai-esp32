// SPDX-License-Identifier: MIT
/* Host stand-in for ESP-SR's esp_aec.h: the types and calls aec_single.c uses. */
#pragma once
#include <stdint.h>

typedef enum {
    AEC_MODE_SR_LOW_COST = 0,
    AEC_MODE_SR_HIGH_PERF = 1,
    AEC_MODE_VOIP_LOW_COST = 3,
    AEC_MODE_VOIP_HIGH_PERF = 4,
    AEC_MODE_FD_LOW_COST = 5,
    AEC_MODE_FD_HIGH_PERF = 6,
} aec_mode_t;

typedef enum {
    AEC_NLP_LEVEL_NORMAL = 0,
    AEC_NLP_LEVEL_AGGR = 1,
    AEC_NLP_LEVEL_VERYAGGR = 2,
} aec_nlp_level_t;

typedef struct {
    int mic_num, ref_num, out_num, filter_length, sample_rate;
    uint32_t caps;
    aec_mode_t mode;
    aec_nlp_level_t nlp_level;
} aec_config_t;

typedef struct aec_handle aec_handle_t;

aec_handle_t *aec_create_from_config(aec_config_t *config);
int aec_get_chunksize(const aec_handle_t *handle);
void aec_process(const aec_handle_t *handle, int16_t *indata, int16_t *refdata, int16_t *outdata);
void aec_destroy(aec_handle_t *handle);
