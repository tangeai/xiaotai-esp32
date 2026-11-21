// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/stream.h"

xiaotai_err_t stream_profile_register(void);
xiaotai_err_t stream_controls_register(void);
const xiaotai_stream_config_t *stream_config(void);
bool stream_has_video(void);
void stream_profile_overflow(void);
void stream_admission_update(void);
void stream_stop_current(void);
