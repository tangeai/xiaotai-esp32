// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media_capture.h"

extern const xiaotai_audio_processor_t media_echo_gate;
void media_echo_gate_set(void *state, bool enabled);
bool media_echo_gate_enabled(void *state);
extern const xiaotai_audio_processor_t media_passthrough;
extern const xiaotai_audio_effect_t media_highpass;
extern const xiaotai_audio_effect_t media_noise_gate;

xiaotai_err_t media_noise_gate_topics_init(void);
/* Called by the uplink owner once per session, before resetting the effect. */
void media_noise_gate_end(void *state);

/* Register the configured native descriptors (none in the portable host build). */
xiaotai_err_t media_platform_register(void);
