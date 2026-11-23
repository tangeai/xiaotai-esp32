// SPDX-License-Identifier: MIT
#pragma once

#include "internal.h"

/* The software decoder wakes its own converter. Hardware decoding converts on
 * its decoder task, with a separate set of retained active transform contexts. */
void video_convert_kick(bool hardware);
void video_convert_task(void *arg);
void video_convert_prepare(void);
void video_convert_reset(void);
