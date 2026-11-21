// SPDX-License-Identifier: MIT
#pragma once
/* Leveled logging with a module tag. Never log on real-time paths (capture,
 * playout, transport callbacks): count instead and report through diagnostics.
 * The linked port supplies the output (ESP-IDF log, or stderr on the host). */
#include "xiaotai_log_port.h"

#define XIAOTAI_LOGE(tag, ...) XIAOTAI_LOG_PORT('E', tag, __VA_ARGS__)
#define XIAOTAI_LOGW(tag, ...) XIAOTAI_LOG_PORT('W', tag, __VA_ARGS__)
#define XIAOTAI_LOGI(tag, ...) XIAOTAI_LOG_PORT('I', tag, __VA_ARGS__)
#define XIAOTAI_LOGD(tag, ...) XIAOTAI_LOG_PORT('D', tag, __VA_ARGS__)
