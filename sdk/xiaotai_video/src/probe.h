// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "xiaotai/hal.h"

/* The console only posts. The camera worker owns every call to the part. */
xiaotai_err_t video_probe_request(void);
bool video_probe_work(bool video_busy, bool (*cancelled)(void));
bool video_probe_blocks_camera(void);
void video_probe_shutdown(void);
void video_probe_reset(void);
uint32_t video_probe_dropped_reports(void);
