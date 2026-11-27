// SPDX-License-Identifier: MIT
#pragma once

#include "esp_afe_sr_iface.h"

/* Only the fields the adapter exchanges with ESP-SR are represented here. */
enum { AFE_TYPE_FD, AFE_MODE_LOW_COST, AFE_MODE_HIGH_PERF, AEC_MODE_FD_LOW_COST, AEC_MODE_FD_HIGH_PERF,
       AEC_NLP_LEVEL_NORMAL, AEC_NLP_LEVEL_AGGR, AEC_NLP_LEVEL_VERYAGGR, AFE_NS_MODE_WEBRTC,
       VAD_MODE_2, AFE_MEMORY_ALLOC_MORE_PSRAM, AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE };
struct afe_config {
    bool aec_init, se_init, ns_init, vad_init, vad_enable_channel_trigger, vad_mute_playback;
    bool wakenet_init, agc_init, fixed_first_channel, fixed_output_channel, output_playback_channel;
    int aec_mode, aec_filter_length, aec_nlp_level, vad_mode, memory_alloc_mode, afe_ns_mode;
    int afe_ringbuf_size, afe_perferred_core, afe_perferred_priority;
    const char *vad_model_name;
    char *ns_model_name;
    float afe_linear_gain;
    struct { int ref_num, mic_num, total_ch_num; } pcm_config;
};

afe_config_t *afe_config_init(const char *format, void *models, int type, int mode);
void afe_config_check(afe_config_t *config);
void afe_config_free(afe_config_t *config);
const esp_afe_sr_iface_t *esp_afe_handle_from_config(const afe_config_t *config);
