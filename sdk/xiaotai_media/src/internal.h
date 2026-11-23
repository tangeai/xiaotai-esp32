// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_INTERNAL_H
#define XIAOTAI_MEDIA_INTERNAL_H

#include <stdbool.h>
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/media_codec.h"
#include "xiaotai/media_capture.h"
#include "topology.h"

typedef struct {
    xiaotai_media_format_t format;
    const xiaotai_audio_decoder_t *decoder;
    void *state;
    size_t max_bytes;
} media_rx_format_t;

/* Prepared entries are immutable until the media tasks and ingress stop. */
bool media_rx_format_select(const xiaotai_media_format_t *wanted, uint8_t *index, media_rx_format_t *prepared);
bool media_rx_format_at(uint8_t index, media_rx_format_t *prepared);
uint32_t media_max_decoder_stack(void);
uint32_t media_prepared_downlink_ms(void);

bool media_provisioning_boot(void);
bool media_stage_initial_controls(void);
xiaotai_err_t media_session_validate(const xiaotai_media_session_t *session);
xiaotai_err_t media_session_runtime_start(const xiaotai_audio_input_t *input, const xiaotai_audio_output_t *output,
                                         xiaotai_downlink_t *downlink, media_topology_t topology);
void media_session_runtime_stop(void);
bool media_session_runtime_failed(void);
void media_session_startup_complete(xiaotai_err_t error);
void media_session_startup_reset(void);
xiaotai_err_t media_session_play(const xiaotai_media_play_t *request, uint32_t *id);
void media_session_stop_play(uint32_t id);
void media_session_wake_playout(void);

#endif
