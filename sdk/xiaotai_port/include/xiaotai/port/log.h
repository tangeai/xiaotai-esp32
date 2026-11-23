// SPDX-License-Identifier: MIT
#pragma once
/* Leveled logging with a module tag. Never log on real-time paths (capture,
 * playout, transport callbacks): count instead and report through diagnostics.
 * The linked port supplies the output (ESP-IDF log, or stderr on the host). */
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai_log_port.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XIAOTAI_LOGE(tag, ...) XIAOTAI_LOG_PORT('E', tag, __VA_ARGS__)
#define XIAOTAI_LOGW(tag, ...) XIAOTAI_LOG_PORT('W', tag, __VA_ARGS__)
#define XIAOTAI_LOGI(tag, ...) XIAOTAI_LOG_PORT('I', tag, __VA_ARGS__)
#define XIAOTAI_LOGD(tag, ...) XIAOTAI_LOG_PORT('D', tag, __VA_ARGS__)

/* Run-time threshold of the system log for every tag, numbered as the console
 * names it: 0 off, 1 error, 2 warning, 3 info, 4 debug. A build compiles
 * messages only up to its ceiling (xiaotai_log_level_max()); a threshold above
 * it prints nothing more. Never saved: every boot starts from the build's
 * default. [any] */
#define XIAOTAI_LOG_LEVEL_DEBUG 4U

/* XIAOTAI_ERR_INVALID_ARG above XIAOTAI_LOG_LEVEL_DEBUG. */
xiaotai_err_t xiaotai_log_set_level(uint8_t level);
uint8_t xiaotai_log_level(void);
uint8_t xiaotai_log_level_max(void);

#ifdef __cplusplus
}
#endif
